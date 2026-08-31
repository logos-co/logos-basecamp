#include "window.h"
#include "logos_api.h"
#include "logos_mode.h"
#include "LogosBasecampPaths.h"
#include "LogRedirector.h"
#include "AccessPolicyOption.h"
#ifdef ENABLE_QML_INSPECTOR
#include "inspectorserver.h"
#endif
#include <QAccessible>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QIcon>
#include <QDir>
#include <QStyleHints>
#include <QStandardPaths>
#include <iostream>
#include <memory>
#include <QStringList>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include "ICoreRuntime.h"
#ifndef LOGOS_MOCK_BACKEND
#include "QtLogosCoreRuntime.h"
#endif
#include "logos_provider_object.h"
#include "qt_provider_object.h"
#include "BuildInfo.h"
#ifdef LOGOS_MOCK_BACKEND
#include "FixtureCoreRuntime.h"
#include "MockBackendFixture.h"
#endif
#ifdef Q_OS_UNIX
#include <QSocketNotifier>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef Q_OS_UNIX
// Self-pipe pattern for SIGTERM/SIGINT: the signal handler writes one byte
// to a socketpair; a QSocketNotifier on the main thread wakes the event loop
// and calls QApplication::quit(), which lets the orderly teardown below
// (~Window, logos_core_cleanup, log flush) run. Doing anything Qt-related
// directly from a signal handler is undefined behaviour.
static int gSignalFd[2] = {-1, -1};

static void unixSignalHandler(int)
{
    char a = 1;
    ::write(gSignalFd[0], &a, sizeof(a));
}

static void installUnixSignalHandlers(QApplication& app)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, gSignalFd) != 0) {
        qWarning() << "Failed to create signal socketpair; SIGTERM/SIGINT will not trigger graceful shutdown";
        return;
    }
    auto* notifier = new QSocketNotifier(gSignalFd[1], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [notifier]() {
        notifier->setEnabled(false);
        char tmp;
        ::read(gSignalFd[1], &tmp, sizeof(tmp));
        QApplication::quit();
    });

    struct sigaction sa {};
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
}
#endif

int main(int argc, char *argv[])
{
    // Set logos mode to Local for testing
    //LogosModeConfig::setMode(LogosMode::Local);

    // Kill the per-file .qmlc disk cache under QStandardPaths::CacheLocation
    // for every QQmlEngine in this process. It must be set before Qt is up:
    // Qt reads the env var when the first engine is constructed, and no later.
    //
    // Rationale: basecamp's own QML modules and the design system are STATIC-
    // embedded via qt_add_qml_module — nothing on disk to cache, so this flag
    // is a no-op for them. The load-bearing effect is on plugin QML under
    // Contents/plugins/<name>/qml/, which ships with nix-frozen mtimes; Qt's
    // (path, mtime + content-hash) cache key can reuse stale .qmlc across app
    // upgrades when a bundled plugin's Q_PROPERTY / signal signatures change
    // between releases. Disabling disk cache costs ~30-100ms of QML parse on
    // the first activation of each plugin per session and makes cross-version
    // plugin upgrades physically immune to that class of staleness
    qputenv("QML_DISABLE_DISK_CACHE", "1");

    // Create QApplication first
    QApplication app(argc, argv);
    app.setOrganizationName("Logos");
    app.setApplicationName("LogosBasecamp");
    app.styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);

    // Inter-module access policy, resolved from the CLI below. Empty ⇒ install
    // nothing (enforcement off) — Basecamp's default, unchanged. See the
    // logos_core_set_access_policy call further down.
    QByteArray accessPolicyJson;

    // Parse --user-dir / -u and set LOGOS_USER_DIR before anything else resolves
    // a path. This lets multiple Basecamp instances run side-by-side against
    // isolated data trees (plugins, modules, module_data, logs). LOGOS_USER_DIR
    // overrides baseDirectory() as-is (no "Dev" suffix), so the user gets the
    // exact path they asked for. parse() rather than process() so unrecognised
    // flags (e.g. Qt's own -platform, -style) don't abort startup.
    {
        QCommandLineParser parser;
        QCommandLineOption userDirOption({"u", "user-dir"},
            QStringLiteral("Override the data directory (isolates plugins, "
                           "modules, module_data, logs for this instance)."),
            QStringLiteral("path"));
        parser.addOption(userDirOption);
        QCommandLineOption accessPolicyOption(QStringLiteral("access-policy"),
            QStringLiteral("Inter-module access policy (default: none, no "
                           "enforcement). 'enforce' turns on deny-by-default: a "
                           "module may only call the modules it declares as "
                           "dependencies. Also accepts a path to a JSON policy "
                           "file, or inline JSON."),
            QStringLiteral("enforce|path|json"));
        parser.addOption(accessPolicyOption);
        if (!parser.parse(app.arguments())) {
            std::cerr << parser.errorText().toStdString() << std::endl;
            return 1;
        }

        // The flag wins; LOGOS_ACCESS_POLICY is the way in for a launch that
        // has no argv to speak of (double-clicked bundle, desktop entry).
        // Neither present ⇒ stays empty ⇒ enforcement off.
        const QString accessPolicyArg = parser.isSet(accessPolicyOption)
            ? parser.value(accessPolicyOption)
            : QString::fromUtf8(qgetenv("LOGOS_ACCESS_POLICY"));
        if (!accessPolicyArg.trimmed().isEmpty()) {
            const auto resolved = LogosBasecamp::resolveAccessPolicy(accessPolicyArg);
            if (!resolved.ok) {
                // Abort rather than boot: the operator explicitly asked to lock
                // this runtime down, and starting anyway would hand them a
                // wide-open one that looks like it obeyed.
                std::cerr << resolved.error.toStdString() << std::endl;
                return 1;
            }
            accessPolicyJson = resolved.policyJson.toUtf8();
        }

        if (parser.isSet(userDirOption)) {
            const QString absUserDir =
                QFileInfo(parser.value(userDirOption)).absoluteFilePath();
            QFileInfo userDirInfo(absUserDir);
            if (userDirInfo.exists() && !userDirInfo.isDir()) {
                qCritical() << "The --user-dir path exists but is not a directory:"
                            << absUserDir;
                return 1;
            }
            if (!userDirInfo.exists() && !QDir().mkpath(absUserDir)) {
                qCritical() << "Failed to create --user-dir directory:"
                            << absUserDir;
                return 1;
            }
            qputenv("LOGOS_USER_DIR", absUserDir.toUtf8());
        }
    }

    // Redirect stdout/stderr to a rotating per-session log file under
    // <baseDirectory>/logs. Must happen after setOrganizationName/setApplicationName
    // and after the --user-dir override is applied so baseDirectory() resolves
    // to the right location. Terminal output is preserved by mirroring to the
    // original stdout.
    const QString logsDir = LogosBasecampPaths::logsDirectory();
    if (!LogosBasecampLog::LogRedirector::instance().start(logsDir)) {
        qWarning() << "Failed to start log redirection; continuing without file logs."
                   << "Logs directory:" << logsDir;
    }

    // Print build metadata (version, dev/portable, commit hashes) so the
    // per-session log captures exactly which sources produced this binary.
    LogosBasecampBuildInfo::logStartupBanner();
    qInfo().noquote() << "Base data directory:" << LogosBasecampPaths::baseDirectory();

#ifdef LOGOS_MOCK_BACKEND
    MockBackendFixture::install();
    {
        const LogosMode mode = LogosModeConfig::getMode();
        if (mode == LogosMode::Mock) {
            qInfo().noquote() << "MockBackendFixture: SDK mode is Mock — no IPC will be attempted.";
        } else {
            qWarning().noquote()
                << "MockBackendFixture: SDK mode is"
                << (mode == LogosMode::Local ? "Local" : "Remote")
                << "but this is a MOCK build. Every module call will attempt real IPC"
                   " and time out. This means logos-protocol did not see"
                   " LOGOS_MOCK_FIXTURE — check that the build picked up the"
                   " logos-protocol you expected.";
        }
    }
#endif

    // Everything liblogos requires BEFORE start() is a constructor argument of
    // the facade, so the ordering constraint below is enforced by the shape of
    // the type rather than by this comment. Applied in the order set here.
    ICoreRuntime::Config coreConfig;

    // Set up module directories for logos core.
    // 1. Embedded modules directory (pre-installed at build time, read-only)
    QString embeddedModulesDir = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../modules");
    coreConfig.modulesDirs.push_back(embeddedModulesDir.toStdString());

    // 2. User-writable modules directory (for runtime installs via the package store)
    QString userModulesDir = LogosBasecampPaths::modulesDirectory();
    coreConfig.modulesDirs.push_back(userModulesDir.toStdString());

    // Set persistence base path for core modules
    coreConfig.persistenceBasePath =
        LogosBasecampPaths::moduleDataDirectory().toStdString();

    // Inter-module access policy. DEFAULT: none — passing NULL clears any
    // policy so no enforcement runs, and any loaded module may call any other.
    //
    // Why off by default: enforce mode's derived deny-by-default gates every
    // ui_qml app's calls to its own backend module, because UI plugins are
    // loaded out-of-process and aren't tracked as dependents in the core
    // ModuleRegistry — so they're never in a module's derived allowed-caller
    // set and get denied (e.g. accounts_ui -> accounts_module). Until the
    // derivation accounts for ui_qml callers, turning this on by default would
    // break the app.
    //
    // Operators can still opt IN per launch with `--access-policy enforce`
    // (or LOGOS_ACCESS_POLICY), and name the ui_qml callers explicitly via a
    // policy document's `restrictions` — see the README.
    // (Must be set before logos_core_start() — see the Config note above.)
    if (accessPolicyJson.isEmpty()) {
        // nullopt, NOT an empty string. The old call passed NULL explicitly,
        // which liblogos turns into "clear the policy"; at startup there is no
        // policy to clear, so declining to set one is the same thing and says
        // what is meant. An empty std::string would take the clearing path.
        coreConfig.accessPolicyJson = std::nullopt;
    } else {
        qInfo().noquote() << "Installing inter-module access policy:" << accessPolicyJson;
        coreConfig.accessPolicyJson = std::string(accessPolicyJson.constData(),
                                                  accessPolicyJson.size());
    }

    // Heap-allocated deliberately. ~LogosCore is what calls
    // logos_core_cleanup(), and it has to run at the explicit reset() during
    // teardown below. A stack object declared here would instead run at the
    // closing brace of main() — silently moving cleanup to AFTER ~LogosAPI,
    // which is destroyed on the same unwind.
    // The ONE place the two backends diverge. Everything downstream —
    // CoreModuleManager, MainUIBackend, the QML — sees only ICoreRuntime and
    // cannot tell which it got.
    std::unique_ptr<ICoreRuntime> core;
#ifdef LOGOS_MOCK_BACKEND
    // install() already ran above — it has to, because exporting
    // LOGOS_MOCK_FIXTURE is what puts logos-protocol's LogosMode into Mock and
    // seeds MockStore, in THIS image and in every child process and plugin
    // image, each of which holds its own copy of those statics.
    core = std::make_unique<FixtureCoreRuntime>(MockBackendFixture::resolvedFixturePath());
    // Built above either way so the two paths differ in one place; a fixture
    // has no directories to load from.
    (void)coreConfig;
#else
    core = std::make_unique<QtLogosCoreRuntime>(argc, argv, std::move(coreConfig));
#endif

    // Start the core
    core->start();
    std::cout << "Logos Core started successfully!" << std::endl;

    bool loaded = core->loadModule(QStringLiteral("package_manager"));

    if (loaded) {
        qInfo() << "package_manager module loaded by default.";
    } else {
        qWarning() << "Failed to load package_manager module by default.";
    }

    bool downloaderLoaded = core->loadModule(QStringLiteral("package_downloader"));
    if (downloaderLoaded) {
        qInfo() << "package_downloader module loaded by default.";
    } else {
        qWarning() << "Failed to load package_downloader module by default.";
    }

    // Log the initial loaded-module list.
    const QStringList modules = core->loadedModules();

    if (modules.isEmpty()) {
        qInfo() << "No modules loaded.";
    } else {
        qInfo() << "Currently loaded modules:";
        for (const QString& name : modules) {
            qInfo() << "  -" << name;
        }
        qInfo() << "Total modules:" << modules.size();
    }

    LogosAPI logosAPI("core", nullptr);

    // Set application icon.
#ifdef Q_OS_LINUX
    // setDesktopFileName is required for Wayland compositors, which look up the
    // icon via the .desktop file name rather than honouring setWindowIcon().
    app.setDesktopFileName("logos-basecamp");
#endif
    app.setWindowIcon(QIcon(":/icons/logos.png"));

    // Don't quit when last window is closed (for system tray support)
    app.setQuitOnLastWindowClosed(false);

#ifdef Q_OS_UNIX
    installUnixSignalHandlers(app);
#endif

    // Create and show the main window. Heap-allocated so we can control
    // destruction ordering explicitly during shutdown (see below).
    auto mainWindow = std::make_unique<Window>(&logosAPI, core.get());
    mainWindow->show();

#ifdef ENABLE_QML_INSPECTOR
    // Start QML Inspector server (controlled by QML_INSPECTOR_PORT env var, default 3768)
    InspectorServer::attach(mainWindow.get());
#endif

    // Run the application
    int result = app.exec();

    // Graceful teardown of the UI before QApplication is destroyed.
    //
    // On macOS, tearing down a QQuickWidget hierarchy crashes inside
    // QCocoaAccessibility::notifyAccessibilityUpdate: QQuickItem destructors
    // call setParentItem(nullptr) which triggers setEffectiveVisibleRecur(false),
    // which notifies the accessibility bridge about items whose backing
    // QObjects are already half-destroyed (null d_ptr → SIGSEGV).
    //
    // Hiding the window alone is insufficient — ~QQuickItem() unconditionally
    // calls setParentItem(nullptr), bypassing the widget visibility state.
    // The fix is to install a no-op accessibility update handler before
    // destroying the widget hierarchy, so the platform bridge is never invoked
    // on partially-destroyed objects.
    if (mainWindow) {
        mainWindow->hide();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();

        // Suppress accessibility notifications during destruction and the
        // subsequent deferred-delete drain. QQuickItem::~QQuickItem() →
        // setParentItem(nullptr) → setEffectiveVisibleRecur →
        // notifyAccessibilityUpdate will hit this no-op instead of the
        // Cocoa bridge. The handler stays suppressed through processEvents()
        // because deleteLater() work queued during destruction can also
        // trigger the same crash path.
        auto previousHandler = QAccessible::installUpdateHandler(
            [](QAccessibleEvent*) {});

        mainWindow.reset();

        // Drain remaining deferred work while the no-op handler is still active.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();

        // Restore the original handler now that all deferred work is done.
        QAccessible::installUpdateHandler(previousHandler);
    }

    // Cleanup logos core (plugins, modules, etc.). ~QtLogosCore calls
    // logos_core_cleanup(); this reset() is what pins it to exactly here,
    // before logosAPI is destroyed on the stack unwind.
    core.reset();

    // Flush final output, restore original stdout/stderr, and close the log file.
    LogosBasecampLog::LogRedirector::instance().stop();

    return result;
}
