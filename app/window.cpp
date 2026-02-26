#include "window.h"
#include <QApplication>
#include <QScreen>
#include <QDebug>
#include <QLabel>
#include <QVBoxLayout>
#include <QPluginLoader>
#include <QDir>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QCloseEvent>
#include <QIcon>
#include <QPixmap>
#include <IComponent.h>
#include <QStandardPaths>
#include <QTimer>
#include "LogosAppPaths.h"
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
    setupUi();
    createTrayIcon();
}

Window::Window(LogosAPI* logosAPI, QWidget *parent)
    : QMainWindow(parent)
    , m_logosAPI(logosAPI)
    , m_trayIcon(nullptr)
    , m_trayIconMenu(nullptr)
    , m_showHideAction(nullptr)
    , m_quitAction(nullptr)
{
    setupUi();
    createTrayIcon();
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

    QString userPluginsDir = LogosAppPaths::pluginsDirectory() + "/";

    // All plugins are installed to the user data directory via preinstall/ lgx packages.
    auto resolvePlugin = [&](const QString& subdir, const QString& name) -> QString {
        return userPluginsDir + subdir + "/" + name + pluginExtension;
    };

    // First, load the package_manager_ui plugin (now in subdirectory)
    QString packageManagerPluginPath = resolvePlugin("package_manager_ui", "package_manager_ui");
    QPluginLoader packageManagerLoader(packageManagerPluginPath);
    QWidget* packageManagerWidget = nullptr;
    
    if (packageManagerLoader.load()) {
        QObject* pmPlugin = packageManagerLoader.instance();
        if (pmPlugin) {
            IComponent* component = qobject_cast<IComponent*>(pmPlugin);
            if (component) {
                packageManagerWidget = component->createWidget(m_logosAPI);
                if (packageManagerWidget) {
                    qDebug() << "Loaded package_manager_ui plugin successfully";
                } else {
                    qWarning() << "package_manager_ui plugin createWidget returned null";
                }
            } else {
                qWarning() << "package_manager_ui plugin does not implement IComponent";
            }
        }
    } else {
        qWarning() << "Failed to load package_manager_ui plugin:" << packageManagerLoader.errorString();
    }

    // Load the main_ui plugin with the appropriate extension (now in subdirectory)
    QString mainUiPluginPath = resolvePlugin("main_ui", "main_ui");
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
        // Pass the package manager widget to main_ui if it was loaded
        if (packageManagerWidget && mainUiPlugin) {
            QMetaObject::invokeMethod(mainUiPlugin, "setPackageManagerWidget",
                                    Qt::DirectConnection,
                                    Q_ARG(QWidget*, packageManagerWidget));
        }
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

    // Set window title and size
    setWindowTitle("Logos App");
    resize(1024, 768);

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
#ifdef Q_OS_MAC
    applyMacWindowRoundedCorners(this);
#endif
}

#ifdef Q_OS_MAC
void Window::setupMacOSDockReopen()
{
    connect(qApp, &QApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive && !isVisible()) {
            show();
            raise();
            activateWindow();
        }
    });
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
    m_trayIcon->setToolTip("Logos App");

    // Create context menu
    m_trayIconMenu = new QMenu(this);

    m_showHideAction = m_trayIconMenu->addAction(tr("Show/Hide"));
    m_showHideAction->setCheckable(false);
    connect(m_showHideAction, &QAction::triggered, this, &Window::showHideWindow);

    m_trayIconMenu->addSeparator();

    m_quitAction = m_trayIconMenu->addAction(tr("Quit"));
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
                tr("Logos App"),
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

void Window::showHideWindow()
{
    if (isVisible()) {
        hide();
    } else {
        show();
        raise();
        activateWindow();
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
