#include "restricted/DenyAllReply.h"

#include <QTimer>
#include <QUrl>

#include "restricted/SandboxLogging.h"

namespace {
const char* operationName(QNetworkAccessManager::Operation op)
{
    switch (op) {
        case QNetworkAccessManager::HeadOperation:   return "HEAD";
        case QNetworkAccessManager::GetOperation:    return "GET";
        case QNetworkAccessManager::PutOperation:    return "PUT";
        case QNetworkAccessManager::PostOperation:   return "POST";
        case QNetworkAccessManager::DeleteOperation: return "DELETE";
        case QNetworkAccessManager::CustomOperation: return "CUSTOM";
        case QNetworkAccessManager::UnknownOperation: break;
    }
    return "UNKNOWN";
}
}

DenyAllReply::DenyAllReply(const QNetworkRequest& request,
                           QNetworkAccessManager::Operation op,
                           QObject* parent,
                           const QString& pluginLabel)
    : QNetworkReply(parent)
{
    setRequest(request);
    setUrl(request.url());
    setOpenMode(QIODevice::ReadOnly);
    setError(QNetworkReply::ContentOperationNotPermittedError,
             QStringLiteral("Network access disabled for this QML engine"));

    qCWarning(lcBasecampSandbox).noquote()
        << QStringLiteral("Blocked network %1 \"%2\"%3: sandboxed ui_qml modules "
                          "may not use the network.")
               .arg(QString::fromLatin1(operationName(op)),
                    request.url().toString(QUrl::RemoveUserInfo),
                    pluginLabel.isEmpty()
                        ? QString()
                        : QStringLiteral(" [plugin=%1]").arg(pluginLabel));

    QTimer::singleShot(0, this, [this]() {
        emit errorOccurred(error());
        emit finished();
    });
}

void DenyAllReply::abort() {}

bool DenyAllReply::isSequential() const
{
    return true;
}

qint64 DenyAllReply::bytesAvailable() const
{
    return 0;
}

qint64 DenyAllReply::readData(char*, qint64)
{
    return -1;
}

qint64 DenyAllReply::writeData(const char*, qint64)
{
    return -1;
}
