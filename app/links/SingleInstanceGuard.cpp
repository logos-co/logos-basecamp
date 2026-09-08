#include "SingleInstanceGuard.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

namespace {

// Long enough to be unambiguous, short enough to stay inside the ~100-character
// limit on a unix socket path once the runtime directory is prepended.
constexpr int kKeyChars = 32;
constexpr int kProbeTimeoutMs = 300;
constexpr int kWriteTimeoutMs = 1000;
constexpr char kTerminator = '\n';

// Guards against a hostile or broken peer streaming forever into the primary.
constexpr int kMaxMessageBytes = 64 * 1024;

} // namespace

SingleInstanceGuard::SingleInstanceGuard(QObject* parent)
    : QObject(parent)
{
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    if (m_server && m_server->isListening())
        m_server->close();
}

QString SingleInstanceGuard::socketNameFor(const QString& userDir)
{
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(userDir.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(kKeyChars));

#ifdef Q_OS_WIN
    return QStringLiteral("logos-basecamp-") + key;
#else
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty() || !QDir().mkpath(dir) || !QFileInfo(dir).isWritable())
        dir = QDir::tempPath();
    return dir + QStringLiteral("/logos-basecamp-") + key;
#endif
}

SingleInstanceGuard::Role SingleInstanceGuard::acquire(const QString& userDir,
                                                       const QString& url)
{
    const QString name = socketNameFor(userDir);
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(kProbeTimeoutMs)) {
        if (!url.isEmpty()) {
            const QByteArray payload = url.toUtf8() + kTerminator;
            probe.write(payload);
            probe.waitForBytesWritten(kWriteTimeoutMs);
        } else {
            probe.write(QByteArray(1, kTerminator));
            probe.waitForBytesWritten(kWriteTimeoutMs);
        }
        probe.flush();
        probe.disconnectFromServer();
        if (probe.state() != QLocalSocket::UnconnectedState)
            probe.waitForDisconnected(kWriteTimeoutMs);

        m_primary = false;
        return Secondary;
    }

    m_server = new QLocalServer(this);

    QLocalServer::removeServer(name);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);

    if (!m_server->listen(name)) {
        qWarning().noquote()
            << "SingleInstanceGuard: could not listen on" << name << "—"
            << m_server->errorString()
            << "; links clicked while this instance is running will start a"
               " second process instead of being handed over.";
        m_primary = true;
        return Primary;
    }

    connect(m_server, &QLocalServer::newConnection,
            this, &SingleInstanceGuard::onNewConnection);
    m_primary = true;
    return Primary;
}

bool SingleInstanceGuard::isPrimary() const
{
    return m_primary;
}

void SingleInstanceGuard::onNewConnection()
{
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        emit secondInstanceDetected();

        m_buffers.insert(socket, QByteArray());

        auto drain = [this, socket]() {
            if (!m_buffers.contains(socket))
                return;
            QByteArray& buffer = m_buffers[socket];
            buffer.append(socket->readAll());

            if (buffer.size() > kMaxMessageBytes) {
                qWarning() << "SingleInstanceGuard: oversized message dropped";
                socket->disconnectFromServer();
                return;
            }

            int newline;
            while ((newline = buffer.indexOf(kTerminator)) >= 0) {
                const QByteArray line = buffer.left(newline);
                buffer.remove(0, newline + 1);
                const QString url = QString::fromUtf8(line);
                if (!url.isEmpty())
                    emit urlReceived(url);
            }
        };

        connect(socket, &QLocalSocket::readyRead, this, drain);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket, drain]() {
            drain();
            m_buffers.remove(socket);
            socket->deleteLater();
        });
        drain();
    }
}
