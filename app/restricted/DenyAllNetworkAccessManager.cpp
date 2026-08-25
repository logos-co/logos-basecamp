#include "restricted/DenyAllNetworkAccessManager.h"

#include "restricted/DenyAllReply.h"

DenyAllNetworkAccessManager::DenyAllNetworkAccessManager(QObject* parent,
                                                         const QString& pluginLabel)
    : QNetworkAccessManager(parent)
    , m_pluginLabel(pluginLabel)
{
}

QNetworkReply* DenyAllNetworkAccessManager::createRequest(Operation op,
                                                          const QNetworkRequest& request,
                                                          QIODevice* outgoingData)
{
    Q_UNUSED(outgoingData);
    return new DenyAllReply(request, op, this, m_pluginLabel);
}
