	#pragma once

#include <QHash>
#include <QObject>
#include <QString>

class QLocalServer;
class QLocalSocket;

// ── SingleInstanceGuard ──────────────────────────────────────────────────────
//
// One Basecamp per user directory, and a way to hand a URL to the one already
// running.
//
// WHY THIS EXISTS AT ALL. On Linux and Windows the OS has no notion of "give
// this to the running app": clicking a `basecamp://` link looks up the
// registered handler and launches a NEW PROCESS with the URL in argv, whether
// or not Basecamp is up. Without this, the second click boots a second runtime
// against the same plugins/ and module_data/. macOS does not need it —
// LaunchServices delivers to the live process as a QFileOpenEvent — but the
// guard runs there too, because nothing otherwise stops a second launch from a
// terminal.
class SingleInstanceGuard : public QObject {
    Q_OBJECT
public:
    enum Role {
        Primary,    // nobody was listening; this process is the app
        Secondary   // another instance answered; `url` was handed over
    };

    explicit SingleInstanceGuard(QObject* parent = nullptr);
    ~SingleInstanceGuard() override;
    Role acquire(const QString& userDir, const QString& url);

    bool isPrimary() const;

signals:
    void urlReceived(const QString& url);
    void secondInstanceDetected();

private:
    void onNewConnection();

    static QString socketNameFor(const QString& userDir);

    QLocalServer* m_server = nullptr;
    bool m_primary = false;
    QHash<QLocalSocket*, QByteArray> m_buffers;
};
