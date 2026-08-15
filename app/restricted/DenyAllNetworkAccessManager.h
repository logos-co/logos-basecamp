#pragma once

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QString>

class QIODevice;

class DenyAllNetworkAccessManager : public QNetworkAccessManager {
public:
    explicit DenyAllNetworkAccessManager(QObject* parent = nullptr,
                                         const QString& pluginLabel = QString());

protected:
    QNetworkReply* createRequest(Operation op,
                                 const QNetworkRequest& request,
                                 QIODevice* outgoingData = nullptr) override;

private:
    QString m_pluginLabel;
};
