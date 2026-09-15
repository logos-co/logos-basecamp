#pragma once

#include <QObject>

class LogosAPI;

class StorageNode : public QObject {
    Q_OBJECT

public:
    explicit StorageNode(LogosAPI* logosAPI, QObject* parent = nullptr);

    void start();
    void shutdown();

signals:
    void stopped();

private:
    void setStorageReady(bool ready);
    void subscribeToStorageEvents();

    LogosAPI* m_logosAPI;
    bool m_subscribed = false;
    bool m_initialised = false;
    bool m_nodeStopped = false;
};
