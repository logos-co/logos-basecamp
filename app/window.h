#ifndef WINDOW_H
#define WINDOW_H

#include <QMainWindow>
#include <QSystemTrayIcon>

class LogosAPI;
class QMenu;
class QAction;
class QCloseEvent;
class QResizeEvent;
class QShowEvent;
class QWidget;

class Window : public QMainWindow
{
    Q_OBJECT

public:
    explicit Window(QWidget *parent = nullptr);
    explicit Window(LogosAPI* logosAPI, QWidget *parent = nullptr);
    ~Window();

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void showHideWindow();
    void iconActivated(QSystemTrayIcon::ActivationReason reason);
    void quitApplication();

private:
    void setupUi();
    void createTrayIcon();
    void setIcon();
    bool isWindowShown() const;
    void restoreWindow();
#ifdef Q_OS_MAC
    void setupMacOSDockReopen();
    void createMenuBar();
#endif

    LogosAPI* m_logosAPI;
    QSystemTrayIcon* m_trayIcon;
    QMenu* m_trayIconMenu;
    QAction* m_showHideAction;
    QAction* m_quitAction;
    QWidget* m_trafficLightsTitleBar = nullptr;
};

#endif // WINDOW_H 
