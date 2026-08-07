#include "window.h"
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QDebug>
#include <QLabel>
#include <QVBoxLayout>
#include <QPluginLoader>
#include "win_dll_search.h"
#include <QDir>
#include <QFile>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QKeySequence>
#include <QShortcut>
#include <QCloseEvent>
#include <QIcon>
#include <QPixmap>
#include <IComponent.h>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>
#include "LogosBasecampPaths.h"
#ifdef Q_OS_MAC
    #include "trafficLightsTitleBar.h"
    #include "macWindowStyle.h"
#endif

Window::Window(QWidget *parent)
    : QMainWindow(parent)
    , m_logosAPI(nullptr)
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

Window::Window(LogosAPI* logosAPI, QWidget *parent)
    : QMainWindow(parent)
    , m_logosAPI(logosAPI)
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
    if (m_trayIcon) {
        delete m_trayIcon;
    }
}

void Window::setupUi()
{
    // Determine the appropriate plugin extension based on the platform
    QString pluginExtension;
    #if defined(Q_OS_WIN)
        pluginExtension = ".dll";
    #elif defined(Q_OS_MAC)
        pluginExtension = ".dylib";
    #else // Linux and other Unix-like systems
        pluginExtension = ".so";
    #endif

    QString embeddedPluginsDir = LogosBasecampPaths::embeddedPluginsDirectory() + "/";
    QString userPluginsDir = LogosBasecampPaths::pluginsDirectory() + "/";

    // Search embedded (pre-installed at build time) first, then user-writable directory.
    auto resolvePlugin = [&](const QString& subdir, const QString& name) -> QString {
        QString embeddedPath = embeddedPluginsDir + subdir + "/" + name + pluginExtension;
        if (QFile::exists(embeddedPath))
            return embeddedPath;
        return userPluginsDir + subdir + "/" + name + pluginExtension;
    };

    // Load the main_ui plugin with the appropriate extension (now in subdirectory)
    QString mainUiPluginPath = resolvePlugin("main_ui", "main_ui");
    // main_ui lives in its own plugins/main_ui/ directory, so anything vendored
    // beside it is invisible to Windows' loader without this. No-op elsewhere;
    // the reference is intentionally held for the process lifetime.
    ModuleLib::preloadPluginWithOwnDirSearch(mainUiPluginPath);
    QPluginLoader loader(mainUiPluginPath);

    QWidget* mainContent = nullptr;
    QObject* mainUiPlugin = nullptr;

    if (loader.load()) {
        mainUiPlugin = loader.instance();
        if (mainUiPlugin) {
            // Try to create the main window using the plugin's createWidget method
            QMetaObject::invokeMethod(mainUiPlugin, "createWidget",
                                    Qt::DirectConnection,
                                    Q_RETURN_ARG(QWidget*, mainContent),
                                    Q_ARG(LogosAPI*, m_logosAPI));
        }
    }

    if (mainContent) {
        setCentralWidget(mainContent);
    } else {
        qWarning() << "================================================";
        qWarning() << "Failed to load main UI plugin from:" << mainUiPluginPath;
        qWarning() << "Error:" << loader.errorString();
        qWarning() << "================================================";
        // Fallback: show a message when plugin is not found
        QWidget* fallbackWidget = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(fallbackWidget);

        QLabel* messageLabel = new QLabel("No main UI module found", fallbackWidget);
        QFont font = messageLabel->font();
        font.setPointSize(14);
        messageLabel->setFont(font);
        messageLabel->setAlignment(Qt::AlignCenter);

        layout->addWidget(messageLabel);
        setCentralWidget(fallbackWidget);
        qWarning() << "Failed to load main UI plugin from:" << mainUiPluginPath;
    }

    // Set window title and size. The default launch width is wide enough for
    // the Package Manager's full table (category sidebar + columns through
    // Action and Description) to be visible without horizontal scrolling; at
    // the old 1024px those rightmost columns were clipped off-screen. The
    // window can still be resized down to MainContainer's 800x600 minimum.
    setWindowTitle("Logos Basecamp");
    {
        // ...but never larger than the screen actually offers. A window bigger
        // than the work area opens with its right and bottom edges off-screen,
        // and on Windows it cannot be dragged back into view, so whatever hangs
        // off is simply unreachable -- here, the sidebar's bottom-anchored
        // system buttons. Measured on a 1271x839-logical RDP session: the
        // 1600x900 request overflowed right by ~342 and bottom by ~97 logical
        // px. Nothing else in this repo consults the screen; the QScreen
        // include at the top of this file has been unused until now.
        //
        // Clamp to availableGeometry EXACTLY, not to a fraction of it. This has
        // to be a no-op on every display big enough to hold the design size --
        // which is all of them in normal use -- or it silently undoes the
        // widening documented above. Qt raises the result back to
        // MainContainer's 800x600 minimum on its own, so there is no second
        // floor to keep in sync here.
        //
        // Skipped under the offscreen platform: QOffscreenScreen hardcodes its
        // geometry to 800x600, so clamping to it would shrink every headless UI
        // doctest window and break qt-mcp's scene-position clicks.
        //
        // NOT handled: a screen that shrinks AFTER launch (an RDP session with
        // /dynamic-resolution). That needs a screenChanged / availableGeometry
        // Changed re-fit, which moves a window out from under the user and is a
        // separate decision.
        QSize target(1600, 900);
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
#ifdef Q_OS_MAC
    if (m_trafficLightsTitleBar && m_trafficLightsTitleBar->isVisible())
        m_trafficLightsTitleBar->setGeometry(0, 0, width(), TrafficLightsTitleBar::kTitleBarHeight);
#endif
}

void Window::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    static bool fitted = false;
    if (!fitted) {
        fitted = true;
        fitFrameToAvailableGeometry();
    }
#ifdef Q_OS_MAC
    applyMacWindowRoundedCorners(this);
#endif
}

void Window::fitFrameToAvailableGeometry()
{
    const QScreen* windowScreen = screen();
    // QOffscreenScreen hardcodes 800x600, so honouring it would shrink every
    // headless UI-doctest window and break qt-mcp's scene-position clicks.
    if (!windowScreen || QGuiApplication::platformName() == QLatin1String("offscreen"))
        return;

    // Compare SIZES, not rectangles, and never touch the position.
    //
    // QRect::contains() is the obvious formulation and is wrong here on two
    // counts. On Windows, frameGeometry() comes from GetWindowRect, which
    // includes DWM's INVISIBLE resize border (~13px per side at 192dpi), so a
    // perfectly placed window never reports as contained and this would run on
    // every single launch. On macOS, Qt hands back a frame whose title bar sits
    // above availableGeometry's origin, so containment fails there too -- and
    // acting on it moved the window 66px down at launch, a visible change on a
    // platform where nothing is broken. Measured both.
    const QSize avail = windowScreen->availableGeometry().size();
    const QSize frame = frameGeometry().size();
    if (frame.width() <= avail.width() && frame.height() <= avail.height())
        return;  // fits -- the normal case, and the only case on macOS/Linux

    // resize() sets the CLIENT size while availableGeometry() bounds the FRAME,
    // so the constructor's clamp is short by the decoration margins -- and those
    // are only knowable once the platform window exists, which is why this runs
    // here and not there. Measured on a 3456x1826 work area: the clamped client
    // filled the work area exactly and the frame still hung 72px below it,
    // putting the sidebar's bottom-anchored system buttons under the taskbar.
    //
    // boundedTo(size()) makes this strictly a shrink. Without it, a window that
    // is merely too TALL also gets stretched to the full available WIDTH --
    // measured on this path: 1600 logical wide silently became 1715.
    const QSize decoration = frame - size();
    resize((avail - decoration).boundedTo(size()).expandedTo(minimumSize()));
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
        // If system tray is not available, quit normally
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
