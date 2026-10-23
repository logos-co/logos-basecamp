#include "restricted/SandboxImages.h"

#include <QBuffer>
#include <QByteArray>
#include <QCache>
#include <QHash>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQuickImageProvider>
#include <QThread>
#include <QQuickTextureFactory>
#include <QRandomGenerator>
#include <QRunnable>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace {

constexpr auto kProviderPrefix = "image://sandbox-media/";

// What all sandboxed apps together hold. Per-app numbers live in the store.
std::atomic<qint64> g_compressedBytes{0};
std::atomic<qint64> g_decodedBytes{0};

QString imageUrl(const QString& id) {
    return QString::fromLatin1(kProviderPrefix) + id;
}

// Is this a data:image/jpeg|png;base64 URL? On success: how long the prefix is
// and which format the prefix promises.
bool splitDataUrl(const QString& url, qsizetype* prefixLength, QByteArray* format) {
    const QString jpeg = QStringLiteral("data:image/jpeg;base64,");
    const QString png  = QStringLiteral("data:image/png;base64,");
    if (url.startsWith(jpeg, Qt::CaseInsensitive)) {
        *prefixLength = jpeg.size();
        *format = "jpeg";
        return true;
    }
    if (url.startsWith(png, Qt::CaseInsensitive)) {
        *prefixLength = png.size();
        *format = "png";
        return true;
    }
    return false;
}

// The URL id: a keyed 64-bit hash of the whole data URL (the key is random per
// app and never shown to QML) plus its length. Cheap to compute and stable, so
// the same picture always maps to the same id. An id only means something
// inside the one engine that made it, so a collision could at worst confuse an
// app with its own pictures.
QString makeId(const QString& dataUrl, quint64 seed) {
    const quint64 hash = quint64(qHash(QStringView(dataUrl), size_t(seed)));
    return QStringLiteral("%1%2")
        .arg(hash, 16, 16, QLatin1Char('0'))
        .arg(quint32(dataUrl.size()), 8, 16, QLatin1Char('0'));
}

struct Entry {
    enum class State { Pending, Ready, Invalid };
    State state = State::Pending;
    QString source;          // the data URL, until a worker has decoded it
    QByteArray bytes;        // the compressed image: kept until release()
    QSize size;              // size as shown (after EXIF rotation): found once
    QString error;           // why it is Invalid
    qint64 cost = 0;         // compressed bytes charged to the budgets
};

// A decoded picture in the cache. Its destructor gives the bytes back to the
// all-apps budget, whether it is evicted, released or the app goes away.
struct DecodedImage {
    DecodedImage(QImage i, qint64 c) : image(std::move(i)), cost(c) {}
    ~DecodedImage() { g_decodedBytes.fetch_sub(cost); }
    QImage image;
    qint64 cost;
};

} // namespace

struct SandboxImageStore {
    SandboxImageStore()
        : decoded(int(std::min<qint64>(SandboxImages::limits().appDecodedBytes / 1024,
                                       std::numeric_limits<int>::max()))) {}
    ~SandboxImageStore() { g_compressedBytes.fetch_sub(compressedBytes); }

    QMutex mutex;
    const quint64 seed = QRandomGenerator::system()->generate64();
    QHash<QString, Entry> entries;
    // key "<id>@<width>x<height>", cost in KiB. Evicting is harmless: the
    // compressed bytes stay in `entries`, so the picture is decoded again.
    QCache<QString, DecodedImage> decoded;
    qint64 compressedBytes = 0;
};

SandboxImages::Limits& SandboxImages::limits() {
    static Limits instance;
    return instance;
}

namespace {

struct Validated {
    bool ok = false;
    QString error;
    QByteArray bytes;
    QSize size;
};

// Worker side: base64, format and dimension checks, BEFORE anything is decoded
// into pixels. Returns the compressed bytes and the size the picture is shown at.
Validated validate(const QString& source) {
    Validated v;
    const SandboxImages::Limits limits = SandboxImages::limits();
    qsizetype prefixLength = 0;
    QByteArray expected;
    if (!splitDataUrl(source, &prefixLength, &expected)) {
        v.error = QStringLiteral("not a JPEG or PNG data URL");
        return v;
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        source.mid(prefixLength).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > limits.maxImageBytes) {
        v.error = QStringLiteral("not valid base64, or too big");
        return v;
    }
    v.bytes = decoded.decoded;

    QBuffer buffer(&v.bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        v.error = QStringLiteral("cannot read the image");
        return v;
    }
    QImageReader reader(&buffer);
    const QByteArray detected = reader.format().toLower();
    if ((expected == "png" && detected != "png") ||
        (expected == "jpeg" && detected != "jpeg" && detected != "jpg")) {
        v.error = QStringLiteral("the bytes are not the format the URL says");
        return v;
    }
    QSize size = reader.size();
    if (!size.isValid() || size.width() > limits.maxDimension ||
        size.height() > limits.maxDimension ||
        qint64(size.width()) * size.height() > limits.maxPixels) {
        v.error = QStringLiteral("the image is too large");
        return v;
    }
    // EXIF orientation 5 to 8 turns a photo on its side: the picture is shown
    // with width and height swapped.
    if (int(reader.transformation()) & int(QImageIOHandler::TransformationRotate90))
        size.transpose();
    v.size = size;
    v.ok = true;
    return v;
}

// What QML will show: no bigger than the picture, no bigger than the largest
// allowed image and no bigger than it asked for. Qt Quick passes 0 or -1 for
// "no preference", and a request is never scaled UP.
QSize displaySize(const QSize& shown, const QSize& requested, int maxDimension) {
    int w = requested.width()  > 0 ? requested.width()  : shown.width();
    int h = requested.height() > 0 ? requested.height() : shown.height();
    w = std::min({w, shown.width(),  maxDimension});
    h = std::min({h, shown.height(), maxDimension});
    QSize target = shown.scaled(QSize(std::max(w, 1), std::max(h, 1)), Qt::KeepAspectRatio);
    return QSize(std::max(target.width(), 1), std::max(target.height(), 1));
}

// Decodes at the display size and turns the picture upright. For JPEG, Qt can
// decode at a reduced size, so a large photo shown small is never held at full size.
QImage decodeAt(const QByteArray& compressed, const QSize& shown, const QSize& target,
                QString* error) {
    QByteArray copy = compressed;  // shares the data; the buffer wants a non-const array
    QBuffer buffer(&copy);
    if (!buffer.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read the image");
        return {};
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    if (target != shown) {
        // The scaled size applies to the picture as stored, before it is turned.
        const bool turned = int(reader.transformation()) & int(QImageIOHandler::TransformationRotate90);
        reader.setScaledSize(turned ? target.transposed() : target);
    }
    QImage image = reader.read();
    if (image.isNull()) {
        *error = reader.errorString();
        return {};
    }
    // Whatever the format did, hand back exactly the size asked for.
    if (image.size() != target)
        image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return image;
}

// One shared pool for every sandboxed app: two threads at most, so untrusted
// pictures can never take the machine's cores. Never destroyed (jobs hold their
// own references to what they use, so exit order does not matter).
QThreadPool* decodePool() {
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(2);
        p->setThreadPriority(QThread::LowPriority);
        return p;
    }();
    return pool;
}


class DecodeJob final : public QRunnable {
public:
    DecodeJob(QSharedPointer<SandboxImageStore> store, QString id, QSize requested,
              QSharedPointer<std::atomic_bool> cancelled, QPointer<QQuickImageResponse> response);
    void run() override;

private:
    QImage work(QString* error);

    QSharedPointer<SandboxImageStore> m_store;
    QString m_id;
    QSize m_requested;
    QSharedPointer<std::atomic_bool> m_cancelled;
    QPointer<QQuickImageResponse> m_response;
};

class SandboxImageResponse final : public QQuickImageResponse {
public:
    SandboxImageResponse(const QSharedPointer<SandboxImageStore>& store, const QString& id,
                  const QSize& requested)
        : m_cancelled(QSharedPointer<std::atomic_bool>::create(false)) {
        auto* job = new DecodeJob(store, id, requested, m_cancelled, QPointer<QQuickImageResponse>(this));
        job->setAutoDelete(true);
        decodePool()->start(job);
    }
    QQuickTextureFactory* textureFactory() const override {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }
    void cancel() override { m_cancelled->store(true); }

    // Runs on the GUI thread (queued from the worker).
    void deliver(const QImage& image, const QString& error) {
        m_image = image;
        m_error = error;
        emit finished();
    }

private:
    QImage m_image;
    QString m_error;
    QSharedPointer<std::atomic_bool> m_cancelled;
};

DecodeJob::DecodeJob(QSharedPointer<SandboxImageStore> store, QString id, QSize requested,
                     QSharedPointer<std::atomic_bool> cancelled, QPointer<QQuickImageResponse> response)
    : m_store(std::move(store)), m_id(std::move(id)), m_requested(requested),
      m_cancelled(std::move(cancelled)), m_response(std::move(response)) {}

void DecodeJob::run() {
    QString error;
    QImage image = work(&error);
    QQuickImageResponse* response = m_response.data();
    if (!response) return;  // the item went away while we worked
    QMetaObject::invokeMethod(
        response,
        [response = m_response, image, error] {
            if (auto* r = static_cast<SandboxImageResponse*>(response.data())) r->deliver(image, error);
        },
        Qt::QueuedConnection);
}

QImage DecodeJob::work(QString* error) {
    const SandboxImages::Limits limits = SandboxImages::limits();
    if (m_cancelled->load()) { *error = QStringLiteral("cancelled"); return {}; }

    // 1. What was kept for this id.
    Entry entry;
    {
        QMutexLocker lock(&m_store->mutex);
        auto it = m_store->entries.constFind(m_id);
        if (it == m_store->entries.constEnd()) { *error = QStringLiteral("unknown or released image"); return {}; }
        entry = *it;
    }
    if (entry.state == Entry::State::Invalid) { *error = entry.error; return {}; }

    // 2. First request: check it and find its size, once.
    if (entry.state == Entry::State::Pending) {
        const Validated v = validate(entry.source);
        QMutexLocker lock(&m_store->mutex);
        auto it = m_store->entries.find(m_id);
        if (it == m_store->entries.end()) { *error = QStringLiteral("unknown or released image"); return {}; }
        it->source.clear();  // the data URL is not needed again
        if (!v.ok) {
            // Remember the verdict, give the budget back.
            it->state = Entry::State::Invalid;
            it->error = v.error;
            m_store->compressedBytes -= it->cost;
            g_compressedBytes.fetch_sub(it->cost);
            it->cost = 0;
            *error = v.error;
            return {};
        }
        it->state = Entry::State::Ready;
        it->bytes = v.bytes;
        it->size = v.size;
        entry = *it;
    }

    // 3. Pick the display size, and reuse a picture already decoded at it.
    const QSize target = displaySize(entry.size, m_requested, limits.maxDimension);
    const QString key = QStringLiteral("%1@%2x%3").arg(m_id).arg(target.width()).arg(target.height());
    {
        QMutexLocker lock(&m_store->mutex);
        if (DecodedImage* cached = m_store->decoded.object(key)) return cached->image;
    }

    // 4. Decode at that size, away from the UI thread and outside the lock.
    if (m_cancelled->load()) { *error = QStringLiteral("cancelled"); return {}; }
    QImage image = decodeAt(entry.bytes, entry.size, target, error);
    if (image.isNull()) return {};

    // 5. Keep it for next time if it fits both budgets (otherwise it is simply
    // decoded again on the next request).
    const qint64 cost = image.sizeInBytes();
    if (cost <= limits.appDecodedBytes) {
        QMutexLocker lock(&m_store->mutex);
        if (m_store->entries.contains(m_id) && !m_store->decoded.contains(key) &&
            g_decodedBytes.load() + cost <= limits.globalDecodedBytes) {
            g_decodedBytes.fetch_add(cost);
            // If the cache refuses it, it deletes it, and the destructor gives the bytes back.
            m_store->decoded.insert(key, new DecodedImage(image, cost), int((cost + 1023) / 1024));
        }
    }
    return image;
}

class SandboxImageProvider final : public QQuickAsyncImageProvider {
public:
    explicit SandboxImageProvider(QSharedPointer<SandboxImageStore> store)
        : m_store(std::move(store)) {}

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override {
        return new SandboxImageResponse(m_store, id, requestedSize);
    }

private:
    QSharedPointer<SandboxImageStore> m_store;
};

} // namespace

SandboxImages::SandboxImages(QObject* parent)
    : QObject(parent), m_store(QSharedPointer<SandboxImageStore>::create()) {}

QQuickAsyncImageProvider* SandboxImages::createProvider() const {
    return new SandboxImageProvider(m_store);
}

QString SandboxImages::sourceFor(const QString& dataUrl) {
    // Only raster image data from the plugin's own existing backend/UI IPC.
    // There is no arbitrary URL loader, file access, or fetch capability here.
    qsizetype prefixLength = 0;
    QByteArray format;
    if (!splitDataUrl(dataUrl, &prefixLength, &format))
        return {};  // includes empty, http:, file:, SVG, script, and other data: URLs

    const Limits lim = limits();
    const qsizetype encodedLength = dataUrl.size() - prefixLength;
    if (encodedLength <= 0 || encodedLength > ((lim.maxImageBytes + 2) / 3) * 4 + 4)
        return {};

    const QString id = makeId(dataUrl, m_store->seed);
    const qint64 cost = qint64(encodedLength) * 3 / 4;  // the compressed size, near enough

    QMutexLocker lock(&m_store->mutex);
    const auto known = m_store->entries.constFind(id);
    if (known != m_store->entries.constEnd())
        return known->state == Entry::State::Invalid ? QString() : imageUrl(id);

    // Over budget: say so by returning nothing. Nothing is decoded or kept.
    if (m_store->entries.size() >= lim.maxImagesPerApp) return {};
    if (m_store->compressedBytes + cost > lim.appCompressedBytes) return {};
    qint64 all = g_compressedBytes.load();
    do {
        if (all + cost > lim.globalCompressedBytes) return {};
    } while (!g_compressedBytes.compare_exchange_weak(all, all + cost));

    m_store->compressedBytes += cost;
    Entry entry;
    entry.source = dataUrl;  // shared, not copied; a worker decodes it on first use
    entry.cost = cost;
    m_store->entries.insert(id, entry);
    return imageUrl(id);
}

void SandboxImages::release(const QString& source) {
    const QString prefix = QString::fromLatin1(kProviderPrefix);
    if (!source.startsWith(prefix)) return;
    const QString id = source.mid(prefix.size());

    QMutexLocker lock(&m_store->mutex);
    auto it = m_store->entries.find(id);
    if (it == m_store->entries.end()) return;
    m_store->compressedBytes -= it->cost;
    g_compressedBytes.fetch_sub(it->cost);
    m_store->entries.erase(it);
    const QString decodedPrefix = id + QLatin1Char('@');
    const QStringList keys = m_store->decoded.keys();
    for (const QString& key : keys)
        if (key.startsWith(decodedPrefix)) m_store->decoded.remove(key);
}