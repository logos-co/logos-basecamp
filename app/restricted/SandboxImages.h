#pragma once

#include <QObject>
#include <QSharedPointer>
#include <QString>

class QQuickImageProvider;
struct SandboxImageCache;

// Per-QML-engine image capability. Sandboxed modules can hand the host an
// inline, bounded JPEG/PNG and receive an opaque image:// URL that only their
// own engine's image provider can resolve. No plaintext files or networking.
class SandboxImages final : public QObject {
    Q_OBJECT
public:
    explicit SandboxImages(QObject* parent = nullptr);
    Q_INVOKABLE QString sourceFor(const QString& dataUrl);
    QQuickImageProvider* createProvider() const;  // ownership passes to QQmlEngine

private:
    QSharedPointer<SandboxImageCache> m_cache;
};
