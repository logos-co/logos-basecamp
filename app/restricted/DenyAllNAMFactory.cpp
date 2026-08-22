#include "restricted/DenyAllNAMFactory.h"

#include "restricted/DenyAllNetworkAccessManager.h"

DenyAllNAMFactory::DenyAllNAMFactory(const QString& pluginLabel)
    : m_pluginLabel(pluginLabel)
{
}

QNetworkAccessManager* DenyAllNAMFactory::create(QObject* parent)
{
    return new DenyAllNetworkAccessManager(parent, m_pluginLabel);
}
