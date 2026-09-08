#include "window.h"
#include "logos_api.h"
#include "logos_mode.h"
#include "LogosBasecampPaths.h"
#include "LogSink.h"
#include "LoggingConfig.h"
#include "AccessPolicyOption.h"
#include "links/LinkUrl.h"
#include "links/LinkUrlInbox.h"
#include "links/SchemeRegistrar.h"
#include "links/SingleInstanceGuard.h"
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

    // Keep Qt's own messages on stderr, where LogSink can capture them.
    //
    // nixpkgs' qtbase links libsystemd, and with journald support compiled in
    // the default message handler routes to the journal INSTEAD of stderr
    // whenever stderr is not a tty — i.e. every launch that is not from a
    // terminal. LogSink captures fd 1 and fd 2, so the session log then holds
    // only what liblogos and the module hosts write directly, with every
    // qInfo/qWarning from the app itself missing and nothing anywhere saying so.
    //
    // Set before Qt logs anything: the tty check behind this is memoised on
    // first use. Left alone if the environment already has an opinion.
    if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING"))
        qputenv("QT_FORCE_STDERR_LOGGING", "1");

    // Create QApplication first
    QApplication app(argc, argv);

    // BEFORE ANYTHING ELSE that can take time. On macOS a `basecamp://` click
    // that launches the app delivers the URL as a QFileOpenEvent very early;
    // a filter installed after the core is up misses it, and then the macOS
    // cold path needs a second mechanism the other platforms do not have.
    // Installing here is what lets every source — this filter, argv, and the
    // single-instance socket — funnel into one inbox.
    LinkUrlInbox::installEventFilter(&app);

    app.setOrganizationName("Logos");
    app.setApplicationName("LogosBasecamp");
    app.styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);

    // Inter-module access policy, resolved from the CLI below. Empty ⇒ install
    // nothing (enforcement off) — Basecamp's default, unchanged. See the
    // logos_core_set_access_policy call further down.
    QByteArray accessPolicyJson;

    // A `basecamp://` URL this process was launched with, read out of the
    // parser block below. Empty for an ordinary launch.
    QString launchUri;

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
        // An explicit option, not a bare positional: the value comes from a
        // scheme handler and is attacker-influenced, and one that happened to
        // look like a flag must not be read as one. Matches what the Windows
        // registry command and the Linux desktop entry pass.
        QCommandLineOption uriOption(QStringLiteral("uri"),
            QStringLiteral("A basecamp:// URL to open. Set by the OS scheme "
                           "handler; not normally typed by hand."),
            QStringLiteral("url"));
        parser.addOption(uriOption);
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

        launchUri = parser.value(uriOption);
    }

    // The resolved session directory. Read once, here, because two separate
    // things key off it and they must agree: the single-instance socket name
    // below, and the logs the sink writes underneath it.
    const QString sessionDir = LogosBasecampPaths::baseDirectory();

    // ── Single instance, per user directory ─────────────────────────────
    //
    // AFTER the --user-dir block, because the socket name is derived from the
    // RESOLVED base directory: --user-dir exists so instances can run side by
    // side deliberately, and a global lock would break exactly that.
    //
    // BEFORE the log sink starts, because a secondary instance lives for about
    // twenty milliseconds and should not leave a rotated per-session log file
    // behind for every link the user clicks.
    //
    // One call, two outcomes. Nobody listening ⇒ this process is the app and
    // starts listening. Someone answers ⇒ the URL has been handed over and
    // there is nothing left to do.
    auto guard = std::make_unique<SingleInstanceGuard>();
    if (guard->acquire(sessionDir, launchUri) == SingleInstanceGuard::Secondary) {
        return 0;
    }

    // This process owns the socket, so it owns the URL it was launched with.
    // Into the inbox rather than acted on: the registry does not know what any
    // app provides until PackageCoordinator's first refresh, seconds from now.
    if (!launchUri.isEmpty())
        LinkUrlInbox::instance().post(launchUri);

    QObject::connect(guard.get(), &SingleInstanceGuard::urlReceived,
                     &app, [](const QString& url) {
                         LinkUrlInbox::instance().post(url);
                     });

    // Capture stdout/stderr — this process's and its module hosts' — into a
    // rotating per-session log file, configured by <session>/config.yaml. Must
    // happen after setOrganizationName/setApplicationName and after the
    // --user-dir override is applied, so the session directory resolves to the
    // right place. Terminal output is preserved by mirroring to the original
    // stdout.
    const LogosBasecampLog::ConfigLoad config = LogosBasecampLog::loadConfig(sessionDir);

    LogosBasecampLog::LogSink::Options logOptions;
    logOptions.enabled   = config.logging.enabled;
    logOptions.dir       = config.logging.dir;
    logOptions.file      = config.logging.file;
    logOptions.maxSizeMb = config.logging.maxSizeMb;
    logOptions.maxFiles  = config.logging.maxFiles;
    logOptions.console   = config.logging.console;
    const bool logStarted = LogosBasecampLog::LogSink::instance().start(logOptions);

    // Reported only now, so anything wrong with the document lands in the log
    // file it configures rather than only on a terminal nobody attached.
    for (const QString& diagnostic : config.diagnostics)
        qWarning().noquote() << "Logging config:" << diagnostic;
    if (!logStarted) {
        qWarning().noquote() << "Could not open a log file under"
                             << config.logging.dir
                             << "— continuing without file logs.";
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

    // Explicit, not the interface default: this path bypasses
    // CoreModuleManager, so nothing else would widen it.
    bool loaded = core->loadModule(QStringLiteral("package_manager"),
                                   LoadPolicy::RequiredAndOptional);

    if (loaded) {
        qInfo() << "package_manager module loaded by default.";
    } else {
        qWarning() << "Failed to load package_manager module by default.";
    }

    bool downloaderLoaded = core->loadModule(QStringLiteral("package_downloader"),
                                             LoadPolicy::RequiredAndOptional);
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

    // Tell the OS that `basecamp://` means this executable. This is what makes
    // the COLD path work — with nothing running, the OS looks up a command line
    // and launches it; the single-instance socket only ever covers the warm
    // path. A no-op on macOS, where Info.plist's CFBundleURLTypes does it.
    //
    // After show() so a first run does not pay for it before anything is on
    // screen, and because a failure here must not stop the app starting.
    if (!SchemeRegistrar::registerScheme()) {
        qWarning() << "Failed to register the" << LinkUrl::scheme()
                   << "URL scheme; links will not open this app.";
    }

    // A second launch — with a URL or not — brings this window forward, so a
    // click that appears to do nothing at least produces motion. Same reason
    // Status calls makeStatusAppActive() on secondInstanceDetected.
    QObject::connect(guard.get(), &SingleInstanceGuard::secondInstanceDetected,
                     mainWindow.get(), [w = mainWindow.get()]() {
                         w->show();
                         w->raise();
                         w->activateWindow();
                     });

    // Bare `basecamp://` raises and nothing else, and a link that DOES carry an
    // intent raises before the consent dialog — the user clicked in a browser
    // and is looking at it, so a dialog on a window that never came forward is
    // worse than a window that then asks.
    mainWindow->setLinkRaiseHandler([w = mainWindow.get()]() {
        w->show();
        w->raise();
        w->activateWindow();
    });

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
    LogosBasecampLog::LogSink::instance().stop();

    return result;
}
