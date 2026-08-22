#pragma once

#include <QNetworkAccessManager>
#include <QQmlNetworkAccessManagerFactory>
#include <QString>

class DenyAllNAMFactory : public QQmlNetworkAccessManagerFactory {
public:
    explicit DenyAllNAMFactory(const QString& pluginLabel = QString());

    QNetworkAccessManager* create(QObject* parent) override;

private:
    QString m_pluginLabel;
};
