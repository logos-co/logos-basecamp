#pragma once

#include <QWidget>

class MainUIBackend;
class LogosAPI;

class MainContainer : public QWidget
{
    Q_OBJECT

public:
    explicit MainContainer(LogosAPI* logosAPI = nullptr, QWidget* parent = nullptr);
    ~MainContainer();

    MainUIBackend* getBackend() const { return m_backend; }
    LogosAPI* getLogosAPI() const { return m_logosAPI; }

private:
    MainUIBackend* m_backend = nullptr;
    LogosAPI* m_logosAPI = nullptr;
};
