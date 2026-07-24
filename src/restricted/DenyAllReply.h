#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>

// Network reply that immediately fails to prevent any QML network usage.
// `pluginLabel` is threaded through so blocked attempts can be logged with the
// plugin they originated from (empty = unlabeled, used by tests).
class DenyAllReply : public QNetworkReply {
public:
    DenyAllReply(const QNetworkRequest& request,
                 QNetworkAccessManager::Operation op,
                 QObject* parent,
                 const QString& pluginLabel = QString());

    void abort() override;
    bool isSequential() const override;
    qint64 bytesAvailable() const override;

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 maxSize) override;
};
