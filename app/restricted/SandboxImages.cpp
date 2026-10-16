#include "restricted/SandboxImages.h"

#include <QBuffer>
#include <QByteArray>
#include <QCache>
#include <QCryptographicHash>
#include <QImage>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QQuickImageProvider>

#include <utility>

namespace {
constexpr qsizetype kMaxCompressedBytes = 6 * 1024 * 1024;
constexpr int kMaxDimension = 4096;
constexpr qint64 kMaxPixels = 16 * 1024 * 1024;
constexpr int kCacheKiB = 64 * 1024;
constexpr auto kProviderPrefix = "image://sandbox-media/";

QString imageUrl(const QString& key) {
    return QString::fromLatin1(kProviderPrefix) + key;
}
} // namespace

struct SandboxImageCache {
    QMutex mutex;
    // Cost is decoded KiB; prevents untrusted QML from retaining unbounded
    // decoded pictures inside Basecamp. Each plugin engine gets its own cache.
    QCache<QString, QImage> images{kCacheKiB};
};

namespace {
class SandboxImageProvider final : public QQuickImageProvider {
public:
    explicit SandboxImageProvider(QSharedPointer<SandboxImageCache> cache)
        : QQuickImageProvider(QQuickImageProvider::Image), m_cache(std::move(cache)) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
        QImage image;
        {
            QMutexLocker lock(&m_cache->mutex);
            const QImage* stored = m_cache->images.object(id);
            if (!stored) return {};
            image = *stored; // QImage is implicitly shared; remains valid after eviction
        }
        if (size) *size = image.size();
        if (requestedSize.isValid()) {
            return image.scaled(requestedSize, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
        }
        return image;
    }

private:
    QSharedPointer<SandboxImageCache> m_cache;
};
} // namespace

SandboxImages::SandboxImages(QObject* parent)
    : QObject(parent), m_cache(QSharedPointer<SandboxImageCache>::create()) {}

QQuickImageProvider* SandboxImages::createProvider() const {
    return new SandboxImageProvider(m_cache);
}

QString SandboxImages::sourceFor(const QString& dataUrl) {
    // Only raster image data from the plugin's own existing backend/UI IPC.
    // There is no arbitrary URL loader, file access, or fetch capability here.
    const QString jpegPrefix = QStringLiteral("data:image/jpeg;base64,");
    const QString pngPrefix  = QStringLiteral("data:image/png;base64,");
    const QString* prefix = nullptr;
    QByteArray expected;
    if (dataUrl.startsWith(jpegPrefix, Qt::CaseInsensitive)) {
        prefix = &jpegPrefix;
        expected = "jpeg";
    } else if (dataUrl.startsWith(pngPrefix, Qt::CaseInsensitive)) {
        prefix = &pngPrefix;
        expected = "png";
    } else {
        return {}; // includes empty, http:, file:, SVG, script, and other data: URLs
    }

    const QString encoded = dataUrl.mid(prefix->size());
    if (encoded.isEmpty() || encoded.size() > ((kMaxCompressedBytes + 2) / 3) * 4 + 4)
        return {};
    const auto decoded = QByteArray::fromBase64Encoding(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > kMaxCompressedBytes) return {};
    const QByteArray bytes = decoded.decoded;
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    {
        QMutexLocker lock(&m_cache->mutex);
        if (m_cache->images.contains(key)) return imageUrl(key);
    }

    // Reject dangerous dimensions BEFORE allocating a decompressed image.
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer);
    const QByteArray detected = reader.format().toLower();
    if ((expected == "png" && detected != "png") ||
        (expected == "jpeg" && detected != "jpeg" && detected != "jpg")) return {};
    const QSize dimensions = reader.size();
    if (!dimensions.isValid() || dimensions.width() > kMaxDimension ||
        dimensions.height() > kMaxDimension ||
        qint64(dimensions.width()) * dimensions.height() > kMaxPixels) return {};
    QImage image = reader.read();
    if (image.isNull()) return {};
    const int cost = qMax(1, int((image.sizeInBytes() + 1023) / 1024));
    if (cost > kCacheKiB) return {};

    {
        QMutexLocker lock(&m_cache->mutex);
        if (!m_cache->images.contains(key))
            m_cache->images.insert(key, new QImage(std::move(image)), cost);
    }
    return imageUrl(key);
}
