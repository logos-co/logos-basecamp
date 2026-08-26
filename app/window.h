#ifndef WINDOW_H
#define WINDOW_H

#include <QMainWindow>
#include <QSystemTrayIcon>
// QPointer<QScreen> needs QScreen COMPLETE -- QPointer static_asserts that its
// argument derives from QObject, so a forward declaration is not enough.
#include <QPointer>
#include <QScreen>
#include <QSize>
#include <QWindow>

class LogosAPI;
class MainUIBackend;
class ShellHostAdapter;
class IShellView;
class QMenu;
class QAction;
class QCloseEvent;
class QResizeEvent;
class QShowEvent;
class QWidget;

// The process-wide core facade, created and owned by main(). Threaded down to
// CoreModuleManager, which is the only thing here that calls it.
namespace logos { namespace qt { class QtLogosCore; } }

class Window : public QMainWindow
{
    Q_OBJECT

public:
    explicit Window(LogosAPI* logosAPI,
                    logos::qt::QtLogosCore* core,
                    QWidget *parent = nullptr);
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
    // Resize the window so its FRAME fits the available geometry of the screen
    // it is on -- shrinking when it does not fit, and growing back toward
    // m_desiredSize when room returns. Runs at first show (frame margins are
    // unknown until the platform window exists, so the constructor's clamp
    // cannot account for them) and again whenever that screen changes under a
    // running window: an RDP session with /dynamic-resolution shrinks the
    // desktop live.
    //
    // EVERY guard lives in here rather than in the callers, so all three entry
    // points are covered by construction. No-op whenever the window already has
    // the right size, which is both the normal case and what makes it
    // non-recursive.
    void fitFrameToAvailableGeometry();
    // Coalesce a burst of screen changes into one fit on a later event-loop
    // turn. An RDP client resizes the desktop continuously while its own window
    // is dragged; fitting each intermediate would chase the drag.
    void scheduleFitToScreen();
    // (Re-)point the screen watch at the screen the window is on now.
    // Idempotent; safe on every show and every screen change.
    void rebindScreenWatch();
    void createTrayIcon();
    void setIcon();
    bool isWindowShown() const;
    void restoreWindow();
#ifdef Q_OS_MAC
    void setupMacOSDockReopen();
    void createMenuBar();
#endif

    LogosAPI* m_logosAPI;
    logos::qt::QtLogosCore* m_core; // not owned; owned by main()

    // Ownership of the UI backend lives HERE, not in the shell. The shell
    // borrows it through m_hostAdapter and can be torn down independently —
    // which is what lets it become a separate image later. Destroyed
    // explicitly and in order by ~Window; see the comment there.
    MainUIBackend*    m_backend    = nullptr;
    ShellHostAdapter* m_hostAdapter = nullptr;
    // The plugin's root instance, reached only through the interface — this
    // image never names MainShellView. Owned by QPluginLoader, not by us.
    IShellView*       m_shellView  = nullptr;
    QSystemTrayIcon* m_trayIcon;
    QMenu* m_trayIconMenu;
    QAction* m_showHideAction;
    QAction* m_quitAction;
    QWidget* m_trafficLightsTitleBar = nullptr;

    // --- screen re-fit ---
    // Held by handle rather than torn down with disconnect(sender, ...) so
    // re-pointing is exact and cannot double-connect.
    // QObject::disconnect(Connection) is null-safe and safe on a connection
    // whose sender was already destroyed.
    QMetaObject::Connection m_screenChangedConnection;
    // The handle the screenChanged connection was made against. Compared, never
    // dereferenced. Tracked explicitly rather than relying on
    // Connection::operator bool going false when its sender is destroyed --
    // Qt documents that only as "the connect() call succeeded", saying nothing
    // about sender lifetime, and a stale-but-truthy handle would silently stop
    // re-pointing the watch.
    QPointer<QWindow> m_watchedWindow;
    QMetaObject::Connection m_availableGeometryConnection;
    // QPointer, not raw: a Windows session lock / RDP disconnect adds and then
    // REMOVES a screen, and handleScreenRemoved() deletes the QScreen. Only ever
    // compared, never dereferenced.
    QPointer<QScreen> m_watchedScreen;
    QTimer* m_fitTimer = nullptr;
    // The size the window is entitled to when there is room -- the launch design
    // size, replaced by whatever the user resizes to. Without it the fit is
    // min(previous, available) on every event, which is monotone non-increasing:
    // the window would converge on the smallest work area seen all session and
    // never grow back.
    QSize m_desiredSize;
    // True only while our own resize is in flight, so resizeEvent can tell a
    // user resize (which redefines m_desiredSize) from ours (which must not).
    // NOT a recursion guard -- the "already the right size" early return is.
    bool m_applyingFit = false;
};

#endif // WINDOW_H 
