#include "window.h"
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QDebug>
#include <QDir>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QKeySequence>
#include <QShortcut>
#include <QCloseEvent>
#include <QIcon>
#include <QPixmap>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>
#include <QPointer>
#include <QScopedValueRollback>
#include "LogosBasecampPaths.h"
#include "MainUIBackend.h"
#include "ShellHostAdapter.h"
#include "IShellHost.h"
#include "IShellView.h"
#include "logos_api.h"
#include "win_dll_search.h"
#include <QPluginLoader>
#ifdef Q_OS_MAC
    #include "trafficLightsTitleBar.h"
    #include "macWindowStyle.h"
#endif

Window::Window(LogosAPI* logosAPI, logos::qt::QtLogosCore* core, QWidget *parent)
    : QMainWindow(parent)
    , m_logosAPI(logosAPI)
    , m_core(core)
    , m_trayIcon(nullptr)
    , m_trayIconMenu(nullptr)
    , m_showHideAction(nullptr)
    , m_quitAction(nullptr)
{
    setObjectName(QStringLiteral("logosMainWindow"));
    setupUi();
    createTrayIcon();
#ifdef Q_OS_MAC
    createMenuBar();
#endif
#ifdef Q_OS_LINUX
    // GNOME/KDE convention: Ctrl+Q quits. QKeySequence::Quit maps to Ctrl+Q
    // on X11/Wayland and is empty on Windows.
    auto* quitShortcut = new QShortcut(QKeySequence::Quit, this);
    quitShortcut->setObjectName(QStringLiteral("logosQuitShortcut"));
    connect(quitShortcut, &QShortcut::activated, this, &Window::quitApplication);
#endif
}

Window::~Window()
{
    // Ordered teardown, stated as a contract instead of left to Qt's reverse
    // child destruction:
    //   1. beginShutdown() unmounts every in-process UI plugin widget while the
    //      shell's widget tree is still intact — those widgets are docked in
    //      the shell, so the shell must outlive this step.
    //   2. destroyShell() detaches the observer before deleting the shell, so
    //      no late host callback reaches a destroyed observer.
    //   3. only then the backend: PackageCoordinator -> UIPluginManager ->
    //      CoreModuleManager, the order MainUIBackend.h documents.
    if (m_backend) {
        m_backend->beginShutdown();
    }

    if (m_shellView) {
        // Out of the central-widget slot before handing it back, or QMainWindow
        // deletes it too.
        QWidget* shell = takeCentralWidget();
        m_shellView->destroyShell(shell);
        // NOT deleted: m_shellView is QPluginLoader's root instance, destroyed
        // by Qt in QLibraryStore::cleanup() at process exit — deleting it here
        // double-frees.
        m_shellView = nullptr;
    }

    delete m_hostAdapter;
    m_hostAdapter = nullptr;

    delete m_backend;
    m_backend = nullptr;

    if (m_trayIcon) {
        delete m_trayIcon;
    }
}

void Window::setupUi()
{
    // The UI shell is a Qt plugin — plugins/main_ui/main_ui.{so,dylib,dll} —
    // reached through IShellView / IShellHost. It gets a QWidget* out and eight
    // named operations in, holds no host privilege, and links no logos runtime;
    // nix/symbol-gate.nix enforces that rather than trusting it.
    //
    // Ownership stays HERE: the backend and its adapter belong to the Window and
    // the shell only borrows them. Deliberately not parented to `this`; ~Window
    // destroys them explicitly and in order.
    m_backend     = new MainUIBackend(m_logosAPI, m_core, nullptr);
    m_hostAdapter = new ShellHostAdapter(m_backend, nullptr);

    QString pluginExtension;
    #if defined(Q_OS_WIN)
        pluginExtension = ".dll";
    #elif defined(Q_OS_MAC)
        pluginExtension = ".dylib";
    #else // Linux and other Unix-like systems
        pluginExtension = ".so";
    #endif

    // Embedded (pre-installed at build time) first, then the user-writable dir.
    const QString embeddedPath = LogosBasecampPaths::embeddedPluginsDirectory()
                               + "/main_ui/main_ui" + pluginExtension;
    const QString mainUiPluginPath =
        QFile::exists(embeddedPath)
            ? embeddedPath
            : LogosBasecampPaths::pluginsDirectory() + "/main_ui/main_ui" + pluginExtension;

    // main_ui lives in its own plugins/main_ui/ directory, so anything vendored
    // beside it is invisible to Windows' loader without this. No-op elsewhere;
    // the reference is intentionally held for the process lifetime.
    ModuleLib::preloadPluginWithOwnDirSearch(mainUiPluginPath);

    QPluginLoader loader(mainUiPluginPath);
    if (!loader.load()) {
        qFatal("Failed to load the main UI plugin from %s: %s",
               qUtf8Printable(mainUiPluginPath), qUtf8Printable(loader.errorString()));
    }

    // A real cast, not QMetaObject::invokeMethod("createWidget"): the string
    // form is a silent-failure path — change an argument type and both sides
    // still compile, then miss at runtime.
    m_shellView = qobject_cast<IShellView*>(loader.instance());
    if (!m_shellView) {
        qFatal("main_ui at %s does not implement IShellView",
               qUtf8Printable(mainUiPluginPath));
    }

    // All that stands between a stale plugin build and a jump through a
    // mismatched vtable slot. Both sides take IShellHost_abi from the one copy
    // in app/interfaces/, so a difference means the plugin was built against a
    // different revision of that header.
    if (m_shellView->hostAbiVersion() != IShellHost_abi) {
        qFatal("main_ui was built against IShellHost ABI %d, host is %d",
               m_shellView->hostAbiVersion(), IShellHost_abi);
    }

    // Deliberately no "No main UI module found" fallback widget: an error-label
    // widget reads as a working app with an empty window. A missing or
    // mismatched shell is a broken install, and the qFatal paths above say
    // which.
    setCentralWidget(m_shellView->createShell(m_hostAdapter));

    // The launch width is sized for the Package Manager's full table (category
    // sidebar through the Action and Description columns) without horizontal
    // scrolling; below it those rightmost columns clip off-screen. Still
    // resizable down to MainContainer's 800x600 minimum.
    setWindowTitle("Logos Basecamp");
    {
        // ...but never larger than the screen offers: an oversized window opens
        // with its right and bottom edges off-screen and, on Windows, cannot be
        // dragged back, so whatever hangs off -- here the sidebar's
        // bottom-anchored system buttons -- is unreachable. Measured on a
        // 1271x839-logical RDP session: 1600x900 overflowed right by ~342 and
        // bottom by ~97 logical px.
        //
        // Clamp to availableGeometry EXACTLY, not to a fraction of it, so this
        // is a no-op on any display big enough for the design size; otherwise it
        // silently undoes the width above. Qt raises the result back to
        // MainContainer's 800x600 minimum on its own.
        //
        // Skipped under the offscreen platform: QOffscreenScreen hardcodes its
        // geometry to 800x600, so clamping to it would shrink every headless UI
        // doctest window and break qt-mcp's scene-position clicks.
        //
        // A screen that shrinks AFTER launch -- an RDP session with
        // /dynamic-resolution -- is handled by rebindScreenWatch() +
        // scheduleFitToScreen() from showEvent: here the window has not landed
        // on a screen yet and its frame margins are not knowable.
        QSize target(1600, 900);
        m_desiredSize = target;  // what to grow back to when room returns
        const QScreen* windowScreen = screen();
        if (windowScreen && QGuiApplication::platformName() != QLatin1String("offscreen"))
            target = target.boundedTo(windowScreen->availableGeometry().size());
        resize(target);
    }

    setAutoFillBackground(true);
    {
        QPalette p = palette();
        p.setColor(QPalette::Window, QColor("#171717"));
        setPalette(p);
    }

#ifdef Q_OS_MAC
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setupMacOSDockReopen();
    // Create title bar after resize() so it gets full width from the start
    m_trafficLightsTitleBar = new TrafficLightsTitleBar(this);
    m_trafficLightsTitleBar->setGeometry(0, 0, width(), TrafficLightsTitleBar::kTitleBarHeight);
    m_trafficLightsTitleBar->show();
    m_trafficLightsTitleBar->raise();
#endif
}

void Window::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    // Leaving maximized/fullscreen/minimized restores a normal-state size that
    // may predate a screen change, so re-check. Scheduled rather than immediate:
    // the macOS branch below deliberately resizes to w-1,h-1 and back via
    // singleShot(0), and the settle delay lands after that has finished.
    if (event->type() == QEvent::WindowStateChange)
        scheduleFitToScreen();
#ifdef Q_OS_MAC
    if (event->type() == QEvent::WindowStateChange) {
        const bool fullScreen = (windowState() & Qt::WindowFullScreen) != 0;
        if (m_trafficLightsTitleBar) {
            if (fullScreen)
                m_trafficLightsTitleBar->hide();
            else
                m_trafficLightsTitleBar->show();
        }
        applyMacWindowRoundedCorners(this, !fullScreen);
        // This is needed to fix squared corners after exiting fullscreen mode
        if (!fullScreen) {
            const int w = width();
            const int h = height();
            resize(w - 1, h - 1);
            QTimer::singleShot(0, this, [this, w, h]() {
                resize(w, h);
                applyMacWindowRoundedCorners(this, true);
            });
        }
    }
#endif
}

void Window::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // A resize the USER performed redefines what the window is entitled to when
    // room returns; one WE performed must not, or the fit becomes
    // min(previous, available) and converges on the smallest work area seen all
    // session. isVisible() keeps Qt's own pre-show sizing out. Maximized and
    // fullscreen sizes belong to the window manager, not to the user's intent:
    // recording one would grow the window to full screen on un-maximize.
    if (!m_applyingFit && isVisible()
        && !(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen)))
        m_desiredSize = size();
#ifdef Q_OS_MAC
    if (m_trafficLightsTitleBar && m_trafficLightsTitleBar->isVisible())
        m_trafficLightsTitleBar->setGeometry(0, 0, width(), TrafficLightsTitleBar::kTitleBarHeight);
#endif
}

void Window::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    // Both idempotent, so no one-shot flag: that is what makes a tray restore
    // re-fit if -- and only if -- the screen shrank while the window was hidden,
    // since the fit is skipped while !isVisible(). A process-wide `static` would
    // gate every Window ever constructed on the first one.
    rebindScreenWatch();
    fitFrameToAvailableGeometry();
#ifdef Q_OS_MAC
    applyMacWindowRoundedCorners(this);
#endif
}

void Window::scheduleFitToScreen()
{
    if (!m_fitTimer) {
        m_fitTimer = new QTimer(this);
        m_fitTimer->setSingleShot(true);
        // An RDP client with /dynamic-resolution resizes the desktop
        // continuously while its own window is dragged. Fitting each
        // intermediate would chase the drag; wait for it to settle. Long enough
        // to outlive a drag frame, short enough to be invisible.
        m_fitTimer->setInterval(150);
        connect(m_fitTimer, &QTimer::timeout, this, &Window::fitFrameToAvailableGeometry);
    }
    m_fitTimer->start();  // restart, collapsing a burst into one fit
}

void Window::rebindScreenWatch()
{
    QWindow* handle = windowHandle();
    if (!handle)
        return;  // not mapped yet; showEvent calls back

    if (handle != m_watchedWindow) {
        QObject::disconnect(m_screenChangedConnection);
        m_watchedWindow = handle;
        m_screenChangedConnection = connect(handle, &QWindow::screenChanged, this,
                                            [this](QScreen*) { rebindScreenWatch(); });
    }

    QScreen* current = handle->screen();
    if (current == m_watchedScreen)
        return;
    // Re-point exactly, by handle: disconnect(sender, ...) would also tear down
    // unrelated connections, and reconnecting without disconnecting would
    // double-fire.
    QObject::disconnect(m_availableGeometryConnection);
    m_watchedScreen = current;
    if (current) {
        m_availableGeometryConnection =
            connect(current, &QScreen::availableGeometryChanged, this,
                    [this](const QRect&) { scheduleFitToScreen(); });
    }
    // Deliberately NO fit here. screenChanged fires when the window MOVES to
    // another monitor -- which the user did on purpose -- and, on Windows, when
    // Qt migrates windows onto the 1024x768 lock screen on RDP disconnect.
    // Fitting then would shrink the window to lock-screen size with no way back.
}

void Window::fitFrameToAvailableGeometry()
{
    // QOffscreenScreen hardcodes its geometry, so honouring it would shrink
    // every headless UI-doctest window and break qt-mcp's scene-position clicks.
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
        return;
    // Basecamp hides to the tray on close, on EVERY platform (see closeEvent),
    // and hide() does not set Qt::WindowMinimized -- so the state check below
    // does not cover it. A hidden window has no meaningful frame geometry and
    // nothing to fit; showEvent re-runs this on restore.
    if (!isVisible())
        return;
    // Maximized and fullscreen sizes belong to the window manager, and a
    // minimized window's geometry is not its restored geometry.
    if (windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized))
        return;
    const QWindow* handle = windowHandle();
    if (!handle)
        return;
    const QScreen* windowScreen = handle->screen();
    if (!windowScreen)
        return;

    // Compare SIZES, not rectangles, and never touch the position.
    // QRect::contains() is wrong here twice over: on Windows frameGeometry()
    // comes from GetWindowRect, which includes DWM's INVISIBLE resize border
    // (~13px per side at 192dpi), so a perfectly placed window never reports as
    // contained and this would run on every launch; on macOS Qt's frame title
    // bar sits above availableGeometry's origin, and acting on that moved the
    // window 66px down at launch. Measured both.
    const QSize avail = windowScreen->availableGeometry().size();

    // resize() sets the CLIENT size while availableGeometry() bounds the FRAME,
    // so the constructor's clamp is short by the decoration margins, knowable
    // only once the platform window exists. Measured on a 3456x1826 work area:
    // the clamped client filled it exactly and the frame still hung 72px below,
    // putting the sidebar's bottom-anchored system buttons under the taskbar.
    const QSize decoration = frameGeometry().size() - size();

    // Bounded by what the window is ENTITLED to, not by its current size: the
    // latter makes this min(previous, available) on every event, ratcheting down
    // to the smallest work area seen all session -- an auto-hiding taskbar, or
    // an RDP window dragged smaller and back, would shrink Basecamp for good.
    // m_desiredSize is the launch design size, replaced by any resize the USER
    // performs (see resizeEvent), so growing back never exceeds what was asked.
    const QSize want = m_desiredSize.isValid() ? m_desiredSize : size();
    const QSize target = (avail - decoration).boundedTo(want).expandedTo(minimumSize());

    // The whole idempotence of this function, and why no re-entrancy flag is
    // needed: our own resize re-enters with the same inputs and stops here.
    if (target == size())
        return;

    QScopedValueRollback<bool> applying(m_applyingFit, true);
    resize(target);
}

#ifdef Q_OS_MAC
void Window::setupMacOSDockReopen()
{
    connect(qApp, &QApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive && !isWindowShown())
            restoreWindow();
    });
}

// Override Qt's default macOS "Quit" (which posts a close event and gets
// swallowed by hide-to-tray). QuitRole promotes this action into the native
// app menu so Cmd+Q actually terminates the process.
void Window::createMenuBar()
{
    QAction* quit = menuBar()->addMenu(tr("File"))->addAction(tr("Quit"));
    quit->setObjectName(QStringLiteral("logosQuitAction"));
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    connect(quit, &QAction::triggered, this, &Window::quitApplication);

    // Frameless on macOS means no native title bar and no Window menu, so
    // nothing is bound to Cmd+M — route it to the traffic light's action.
    // Qt maps Ctrl to Command here (see Qt::AA_MacDontSwapCtrlAndMeta).
    QAction* minimize = menuBar()->addMenu(tr("Window"))->addAction(tr("Minimize"));
    minimize->setObjectName(QStringLiteral("logosMinimizeAction"));
    minimize->setShortcut(QKeySequence(QStringLiteral("Ctrl+M"))); // Cmd+M on macOS
    minimize->setMenuRole(QAction::NoRole);
    connect(minimize, &QAction::triggered, this, &QWidget::showMinimized);
}
#endif

void Window::createTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qWarning() << "System tray is not available on this system";
        return;
    }

    // Create tray icon
    m_trayIcon = new QSystemTrayIcon(this);
    setIcon();
    m_trayIcon->setToolTip("Logos Basecamp");

    // Create context menu
    m_trayIconMenu = new QMenu(this);

    m_showHideAction = m_trayIconMenu->addAction(tr("Show/Hide"));
    m_showHideAction->setObjectName(QStringLiteral("logosTrayShowHideAction"));
    m_showHideAction->setCheckable(false);
    connect(m_showHideAction, &QAction::triggered, this, &Window::showHideWindow);

    m_trayIconMenu->addSeparator();

    m_quitAction = m_trayIconMenu->addAction(tr("Quit"));
    m_quitAction->setObjectName(QStringLiteral("logosTrayQuitAction"));
    connect(m_quitAction, &QAction::triggered, this, &Window::quitApplication);

    m_trayIcon->setContextMenu(m_trayIconMenu);

    // Connect icon activation signal
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &Window::iconActivated);

    // Show the tray icon
    m_trayIcon->show();
}

void Window::setIcon()
{
    if (!m_trayIcon) {
        return;
    }

    QIcon icon(":/icons/logos.png");
    if (icon.isNull()) {
        qWarning() << "Failed to load tray icon from resource";
        // Create a simple fallback icon
        icon = QIcon::fromTheme("application-x-executable");
        if (icon.isNull()) {
            // Last resort: create a minimal icon
            QPixmap pixmap(16, 16);
            pixmap.fill(Qt::blue);
            icon = QIcon(pixmap);
        }
    }
    m_trayIcon->setIcon(icon);
}

void Window::closeEvent(QCloseEvent *event)
{
#ifdef Q_OS_MAC
    // In full screen, close only exits full screen (Discord-style); do not hide
    if (windowState() & Qt::WindowFullScreen) {
        setWindowState(Qt::WindowNoState);
        event->ignore();
        return;
    }
#endif

    if (m_trayIcon && m_trayIcon->isVisible()) {
        // Hide the window instead of closing
        hide();
        event->ignore();
        
        // Show a message to inform the user
        if (m_trayIcon->supportsMessages()) {
            m_trayIcon->showMessage(
                tr("Logos Basecamp"),
                tr("The application will continue to run in the system tray. "
                   "Click the tray icon to restore the window."),
                QSystemTrayIcon::Information,
                2000
            );
        }
    } else {
        // No tray: close only hides — setQuitOnLastWindowClosed(false) keeps the app running.
        event->accept();
    }
}

bool Window::isWindowShown() const
{
    // isVisible() alone is not enough: it stays true when minimized (#268), and
    // on Wayland only exposure reveals that. Exposure can also drop for an
    // occluded window, which then raises instead of hiding — the harmless miss.
    const QWindow* handle = windowHandle();
    if (!isVisible() || isMinimized() || (handle && !handle->isExposed()))
        return false;
#ifdef Q_OS_MAC
    return !macAppIsHidden();
#else
    return true;
#endif
}

void Window::restoreWindow()
{
    // macOS: order back in first, or show() re-applies WindowMinimized.
    if (!isVisible())
        show();
    // Clear only the minimized bit, so maximized/fullscreen survives.
    if (isMinimized())
        setWindowState(windowState() & ~Qt::WindowMinimized);

#ifdef Q_OS_MAC
    macDeminiaturize(this);
    macActivateApp();
#else
    // The state bit is advisory on X11 and a no-op on Wayland (no unminimize
    // request exists), so force a fresh map — that every display server honours.
    if (windowHandle() && !windowHandle()->isExposed()) {
        hide();
        show();
    }
#endif
    raise();
    activateWindow();
}

void Window::showHideWindow()
{
    if (isWindowShown()) {
        hide();
    } else {
        restoreWindow();
    }
}

void Window::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
        // Single click - toggle window visibility
        showHideWindow();
        break;
    case QSystemTrayIcon::DoubleClick:
        // Double click - also toggle (some platforms use double click)
        showHideWindow();
        break;
    case QSystemTrayIcon::MiddleClick:
        // Middle click - toggle as well
        showHideWindow();
        break;
    default:
        break;
    }
}

void Window::quitApplication()
{
    // Hide tray icon first
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    
    // Quit the application
    QApplication::quit();
} 
