#pragma once

#include <QObject>
#include <QSharedPointer>
#include <QString>

class QQuickAsyncImageProvider;
struct SandboxImageStore;

// Per-QML-engine image capability. Sandboxed modules can hand the host an
// inline, bounded JPEG/PNG and receive an opaque image:// URL that only their
// own engine's image provider can resolve. No plaintext files or networking.
//
// sourceFor() only checks the prefix and length, charges the budgets and
// returns at once. Base64 decoding, validation and image decoding all happen
// on a small shared worker pool when the image is first requested, at the size
// the item is displayed (never larger than the image itself).
class SandboxImages final : public QObject {
    Q_OBJECT
public:
    // Every number a reviewer may want to tune. The defaults are the budgets
    // asked for in review: 64 MB compressed and 64 MB decoded per app, 192 MB
    // compressed and 128 MB decoded across all of Basecamp. Process-wide;
    // change them before the first engine is configured (tests do).
    struct Limits {
        qint64 appCompressedBytes    = 64LL * 1024 * 1024;
        qint64 appDecodedBytes       = 64LL * 1024 * 1024;
        qint64 globalCompressedBytes = 192LL * 1024 * 1024;
        qint64 globalDecodedBytes    = 128LL * 1024 * 1024;
        qint64 maxImageBytes         = 6LL * 1024 * 1024;   // one image, compressed
        int    maxDimension          = 4096;                // either side, pixels
        qint64 maxPixels             = 16LL * 1024 * 1024;  // width * height
        int    maxImagesPerApp       = 4096;                // entries, valid or not
    };
    static Limits& limits();

    explicit SandboxImages(QObject* parent = nullptr);

    // Returns the image:// URL for a data:image/jpeg|png;base64 URL, or an empty
    // string if it is not a JPEG/PNG data URL, is too big, or this app (or
    // Basecamp as a whole) is over its budget. Calling it again with the same
    // string is cheap and returns the same URL.
    Q_INVOKABLE QString sourceFor(const QString& dataUrl);

    // Frees what sourceFor() kept for a URL it returned (the compressed bytes
    // and any decoded copies). Call it when the last item showing it goes away.
    Q_INVOKABLE void release(const QString& source);

    QQuickAsyncImageProvider* createProvider() const;  // ownership passes to QQmlEngine

private:
    QSharedPointer<SandboxImageStore> m_store;
};