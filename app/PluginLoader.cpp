#include "PluginLoader.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QMutexLocker>
#include <QPluginLoader>
#include "win_dll_search.h"
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickWidget>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <memory>

#include "CoreDependencyLoader.h"
#include "CoreModuleManager.h"
#include "IComponent.h"
#include "IntentBridgeAdapter.h"
#include "LogosQmlBridge.h"
#include "logos_api.h"
#include "logos_consumer.h"
#include "restricted/QmlSandbox.h"
#include "utils/DependencyEntry.h"
#include <ViewModuleHost.h>

PluginLoader::PluginLoader(LogosAPI* logosAPI,
                           CoreModuleManager* coreModuleManager,
                           QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
    , m_coreModuleManager(coreModuleManager)
{
    m_depLoader = new CoreDependencyLoader(this);
}

namespace {

// From ui-host's READY to its backend replica being Valid: one round trip for
// the schema, answered even while the backend waits on synchronous IPC. Only a
// wedged backend reaches this; one that exits fails the load at once.
constexpr int kBackendReadyTimeoutMs = 30000;

// Two frames of the primary screen's refresh, floored at one 60 Hz frame.
int spinnerPaintDelayMs()
{
    qreal hz = 0;
    if (const QScreen* screen = QGuiApplication::primaryScreen())
        hz = screen->refreshRate();
    if (hz < 1.0) hz = 60.0;
    return qBound(16, static_cast<int>(std::ceil(2000.0 / hz)), 50);
}

}  // namespace

void PluginLoader::load(const PluginLoadRequest& request)
{
    if (isLoading(request.name)) {
        qDebug() << "Plugin" << request.name << "is already loading";
        return;
    }

    setLoading(request.name, true);

    // Let the spinner reach the screen before anything blocks. A zero timer
    // does not: measured on Qt 6.9.2, singleShot(0) paints ZERO frames — and
    // so does four of them nested, because zero timers all drain in one
    // event-loop pass while a QQuickWidget's first paint lands a frame later.
    QTimer::singleShot(spinnerPaintDelayMs(), this, [this, request]() {
        startLoad(request);
    });
}

bool PluginLoader::isLoading(const QString& name) const
{
    QMutexLocker lock(&m_mutex);
    return m_loading.contains(name);
}

QStringList PluginLoader::loadingPlugins() const
{
    QMutexLocker lock(&m_mutex);
    return m_loading.values();
}

void PluginLoader::setLoading(const QString& name, bool loading)
{
    {
        QMutexLocker lock(&m_mutex);
        if (loading)
            m_loading.insert(name);
        else
            m_loading.remove(name);
    }
    emit loadingChanged();
}

logos::ConsumerIdentity PluginLoader::consumerFor(const QString& name)
{
    if (name.isEmpty()) {
        qWarning() << "PluginLoader: refusing to build an identity for an unnamed plugin";
        return {};
    }

    auto it = m_consumers.constFind(name);
    if (it != m_consumers.constEnd())
        return it.value();

    // logos::admitConsumer isolates the store, constructs the LogosAPI on it,
    // mints a credential, registers it with capability_module over the HOST's
    // trusted channel, and only then installs it in the identity's store. The
    // ordering notes that used to live here — isolate before constructing
    // anything (a LogosAPIClient captures its store as a raw pointer), and
    // register synchronously before the plugin's first QTimer::singleShot(0)
    // call can fire — are now stated once, next to the implementation.
    logos::ConsumerIdentity consumer = logos::admitConsumer(name, m_logosAPI, this);
    if (!consumer) {
        qWarning() << "PluginLoader: could not admit" << name
                   << "as a consumer - refusing to load it with the host's authority";
        return {};
    }
    m_consumers.insert(name, consumer);

    return consumer;
}

void PluginLoader::startLoad(const PluginLoadRequest& request)
{
    if (request.coreDependencies.isEmpty() && request.optionalCoreDependencies.isEmpty()) {
        continueLoad(request);
        return;
    }

    loadCoreDependencies(request);
}

void PluginLoader::loadCoreDependencies(const PluginLoadRequest& request)
{
    // Resolve the names here, on the GUI thread: a malformed manifest is a
    // caller error and should be reported without spawning anything.
    QStringList depNames;
    for (const QVariant& dep : request.coreDependencies) {
        // Bare name or {"name": …, "version": …, "signer": …} — either way we
        // need the name. See utils/DependencyEntry.h for why this must not be
        // a bare dep.toString().
        const logos::DependencyEntry entry = logos::readDependencyEntry(dep);
        if (entry.kind == logos::DependencyEntryKind::Unrecognised) {
            // No name means no way to honour the declaration. Refuse rather
            // than continue: mounting the plugin with a declared dependency
            // silently unloaded surfaces later as a missing method or a dead
            // endpoint, with nothing in the log pointing here.
            qWarning() << "Unrecognised core dependency entry" << dep
                       << "for" << request.name
                       << "- expected a module name, or an object with a "
                          "string \"name\"; refusing to load";
            setLoading(request.name, false);
            emit pluginLoadFailed(request.name,
                QStringLiteral("Unrecognised dependency entry in ")
                    + request.name + QStringLiteral("'s manifest"));
            return;
        }
        depNames << entry.name;
    }

    // Optional dependencies: every failure here is a warning, never a refusal.
    // The plugin declared it does not need these, so an absent or broken one
    // must not stop it mounting -- the same rule the blocking gate follows.
    QStringList optionalDepNames;
    for (const QVariant& dep : request.optionalCoreDependencies) {
        const logos::DependencyEntry entry = logos::readDependencyEntry(dep);
        if (entry.kind == logos::DependencyEntryKind::Unrecognised) {
            qWarning() << "Unrecognised optional dependency entry" << dep
                       << "for" << request.name << "- skipping";
            continue;
        }
        optionalDepNames << entry.name;
    }

    qDebug() << "Loading core dependencies for" << request.name << ":" << depNames;

    // Off the GUI thread — see CoreDependencyLoader. continueLoad still runs
    // here, after every dependency has reported, so the steps below it keep
    // the ordering they had when this loop was synchronous.
    //
    // The load callable takes the runtime BY VALUE rather than `this`: it runs
    // on the worker, which must not read a PluginLoader that is already
    // part-way through destruction when its loader joins.
    m_depLoader->enqueue(depNames, optionalDepNames,
        [core = m_coreModuleManager, name = request.name](const QString& dep, bool required) {
            if (!core)
                return false;
            if (!required)
                qDebug() << "Loading optional dependency for" << name << ":" << dep;
            if (core->loadModule(dep))
                return true;
            if (!required) {
                qInfo() << "Optional dependency" << dep << "for" << name
                        << "is unavailable; continuing without it";
            }
            return false;
        },
        [this, request]() {
            continueLoad(request);
        },
        [this, request](const QString& failedDependency) {
            qWarning() << "Failed to load core dependency" << failedDependency
                       << "for" << request.name;
            setLoading(request.name, false);
            emit pluginLoadFailed(request.name,
                QStringLiteral("Failed to load core dependencies for ") + request.name);
        });
}

void PluginLoader::continueLoad(const PluginLoadRequest& request)
{
    switch (request.type) {
    case UIPluginType::UiQml:
        loadUiQmlModule(request);
        break;
    case UIPluginType::Legacy:
        loadCppPluginAsync(request);
        break;
    }
}

// ---------- Legacy ui plugin path ----------

void PluginLoader::loadCppPluginAsync(const PluginLoadRequest& request)
{
    // Pre-load the shared library in a background thread.
    // Qt's QLibraryStore caches loaded libraries globally, so the subsequent
    // QPluginLoader::load() on the main thread will be instant.
    QThread* thread = QThread::create([path = request.pluginPath]() {
        // Add the plugin's OWN directory to the DLL search before the image is
        // mapped, so a UI plugin can resolve libraries vendored beside it.
        // Windows searches the EXECUTABLE's directory, not the importing DLL's.
        // No-op off Windows. The reference is deliberately not released: these
        // plugins stay resident for the process lifetime (QPluginLoader's
        // destructor does not unload), and this is the load that actually maps
        // the image -- the main-thread load below then hits Qt's cache.
        ModuleLib::preloadPluginWithOwnDirSearch(path);
        QPluginLoader loader(path);
        loader.load();
    });

    connect(thread, &QThread::finished, this,
        [this, thread, request]() {
            thread->deleteLater();
            finishCppPluginLoad(request);
        });

    thread->start();
}

void PluginLoader::finishCppPluginLoad(const PluginLoadRequest& request)
{
    // Normally a no-op: loadCppPluginAsync already mapped the image. Kept so
    // this path is correct on its own, since a caller reaching it without the
    // async pre-load would otherwise fail to resolve vendored DLLs.
    ModuleLib::preloadPluginWithOwnDirSearch(request.pluginPath);
    QPluginLoader loader(request.pluginPath);
    if (!loader.load()) {
        qWarning() << "Failed to load plugin:" << request.name << "-" << loader.errorString();
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name, loader.errorString());
        return;
    }

    QObject* plugin = loader.instance();
    if (!plugin) {
        qWarning() << "Failed to get plugin instance:" << request.name;
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name, QStringLiteral("Failed to get plugin instance"));
        return;
    }

    IComponent* component = qobject_cast<IComponent*>(plugin);
    if (!component) {
        qWarning() << "Plugin does not implement IComponent:" << request.name;
        loader.unload();
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name, QStringLiteral("Plugin does not implement IComponent"));
        return;
    }

    // The plugin's own identity, not the host's. A legacy widget plugin calls
    // modules through whatever LogosAPI it is handed, so handing it m_logosAPI
    // handed it the host's ambient token ring.
    //
    // One call where there were two. The old pair minted a UUID, registered it,
    // and then DROPPED it — the identity was registered under a credential
    // nobody held, and it worked only because the isolated store had been born
    // holding a copy of the host's anchor. That copy is gone; the credential is
    // now installed in the store admitConsumer built.
    const logos::ConsumerIdentity consumer = consumerFor(request.name);
    if (!consumer) {
        loader.unload();
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("Could not establish an isolated identity for ") + request.name);
        return;
    }

    QWidget* widget = component->createWidget(consumer.api);
    if (!widget) {
        qWarning() << "Component returned null widget:" << request.name;
        loader.unload();
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name, QStringLiteral("Component returned null widget"));
        return;
    }

    if (!request.iconPath.isEmpty())
        widget->setWindowIcon(QIcon(request.iconPath));

    setLoading(request.name, false);
    emit pluginLoaded(request.name, widget, component, UIPluginType::Legacy, nullptr);
}

// ---------- ui_qml module path ----------

void PluginLoader::loadUiQmlModule(const PluginLoadRequest& request)
{
    if (request.qmlViewPath.isEmpty() || !QFile::exists(request.qmlViewPath)) {
        qWarning() << "ui_qml module QML file not found:" << request.qmlViewPath;
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("QML view file not found: ") + request.qmlViewPath);
        return;
    }

    // The bridge the QML gets speaks AS this module, from this module's own
    // token store — not as basecamp.
    //
    // ONE ADMISSION, ONE CREDENTIAL, BOTH PATHS. Two separate things used to be
    // conflated here, and only the second one needs a ui-host:
    //
    //   1. Making `request.name` a KNOWN CALLER at capability_module, which is
    //      what lets its very first requestModule get past the known-caller
    //      gate. Needed by the pure-QML path too — in fact especially there,
    //      since the QML is the only thing calling out.
    //   2. Giving ui-host the credential its backend accepts inbound calls with.
    //
    // The old code did (1) inside the has-a-backend branch, below an early
    // return, so the pure-QML path registered nothing at all. It got away with
    // it because it was calling with the host's ambient ring, where every
    // target's token was already present and no handshake ever happened. Both
    // are now one call that cannot be branched around, and the credential it
    // returns is the SAME value in the store and on the wire to ui-host —
    // where it used to be minted here and never given to the in-process bridge.
    const logos::ConsumerIdentity consumer = consumerFor(request.name);
    if (!consumer) {
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("Could not establish an isolated identity for ") + request.name);
        return;
    }
    auto* bridge = new LogosQmlBridge(consumer.api, this);

    if (request.mainFilePath.isEmpty()) {
        loadQmlView(request, bridge, nullptr);
        return;
    }

    // Has a backend plugin — spawn a ViewModuleHost process.
    //
    // The credential goes to ui-host, which adopts it into its own image's
    // token store (logos::adoptConsumerCredential). admitConsumer registered it
    // synchronously above, before this process spawns, which is the race the
    // old comment here described: plugin ctors commonly schedule their first
    // IPC via QTimer::singleShot(0, ...) and those fire the instant ui-host
    // enters its event loop, so a registration deferred to onHostReady would
    // lose to them and capability_module would refuse with "auth token not
    // recognized".
    auto* viewHost = new ViewModuleHost(this);
    if (!viewHost->spawn(request.name, request.mainFilePath, consumer.credential)) {
        qWarning() << "Failed to spawn ui-host for ui_qml module" << request.name;
        delete viewHost;
        delete bridge;
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("Failed to spawn ui-host for ") + request.name);
        return;
    }

    auto onHostReady = [this, request, bridge, viewHost]() {
        bridge->setViewModuleSocket(request.name, viewHost->socketName(),
                                    viewHost->sourceName());
        auto exitConn = std::make_shared<QMetaObject::Connection>(
            connect(viewHost, &ViewModuleHost::processExited, bridge,
                [bridge, name = request.name](int) { bridge->notifyViewModuleCrashed(name); }));

        bridge->prepareViewModule(request.name, kBackendReadyTimeoutMs,
            [this, request, bridge, viewHost, exitConn](bool ok, const QString& error) {
                QObject::disconnect(*exitConn);
                if (!ok) {
                    qWarning() << "Backend of" << request.name << "not ready:" << error;
                    viewHost->stop();
                    viewHost->deleteLater();
                    delete bridge;
                    setLoading(request.name, false);
                    emit pluginLoadFailed(request.name, error);
                    return;
                }
                loadQmlView(request, bridge, viewHost);
            });
    };

    auto* timeout = new QTimer(this);
    timeout->setSingleShot(true);
    auto readyConn = std::make_shared<QMetaObject::Connection>();
    auto timeoutConn = std::make_shared<QMetaObject::Connection>();
    *readyConn = connect(viewHost, &ViewModuleHost::ready, this,
        [timeout, readyConn, timeoutConn, onHostReady]() {
            QObject::disconnect(*readyConn);
            QObject::disconnect(*timeoutConn);
            timeout->stop();
            timeout->deleteLater();
            onHostReady();
        });
    *timeoutConn = connect(timeout, &QTimer::timeout, this,
        [this, request, viewHost, bridge, timeout, readyConn, timeoutConn]() {
            QObject::disconnect(*readyConn);
            QObject::disconnect(*timeoutConn);
            timeout->deleteLater();
            qWarning() << "Timeout waiting for ui-host ready signal for" << request.name;
            viewHost->stop();
            viewHost->deleteLater();
            delete bridge;
            setLoading(request.name, false);
            emit pluginLoadFailed(request.name,
                QStringLiteral("Timeout waiting for ui-host for ") + request.name);
        });
    timeout->start(30000);
}

void PluginLoader::loadQmlView(const PluginLoadRequest& request,
                               LogosQmlBridge* bridge,
                               ViewModuleHost* viewHost)
{
    auto* qmlWidget = new QQuickWidget;
    qmlWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    if (QQmlEngine* engine = qmlWidget->engine()) {
        const QString appLibDir =
            QDir(QCoreApplication::applicationDirPath() + "/../lib").canonicalPath();
        QmlSandbox::configure(engine, request.installDir, request.qmlViewPath,
                              appLibDir, request.name);
        engine->setBaseUrl(QUrl::fromLocalFile(request.installDir + "/"));
    }

    // Async pre-compile: the engine caches compiled types so setSource() is fast.
    QUrl sourceUrl = QUrl::fromLocalFile(request.qmlViewPath);
    auto* preloader = new QQmlComponent(qmlWidget->engine(), sourceUrl,
                                        QQmlComponent::Asynchronous);

    auto finishOrCleanup = [this, preloader, qmlWidget, request, bridge,
                            viewHost](QQmlComponent::Status status) {
        preloader->deleteLater();
        if (status == QQmlComponent::Ready) {
            finishUiQmlLoad(qmlWidget, request, bridge, viewHost);
        } else {
            QString errors;
            for (const auto& e : preloader->errors())
                errors += e.toString() + QStringLiteral("\n");
            qWarning() << "Failed to compile ui_qml view" << request.name << ":" << errors;
            qmlWidget->deleteLater();
            delete bridge;
            if (viewHost) { viewHost->stop(); delete viewHost; }
            setLoading(request.name, false);
            emit pluginLoadFailed(request.name, errors);
        }
    };

    if (preloader->isReady() || preloader->isError()) {
        finishOrCleanup(preloader->status());
    } else {
        connect(preloader, &QQmlComponent::statusChanged, this, finishOrCleanup);
    }
}

void PluginLoader::finishUiQmlLoad(QQuickWidget* qmlWidget,
                                   const PluginLoadRequest& request,
                                   LogosQmlBridge* bridge,
                                   ViewModuleHost* viewHost)
{
    // The engine, not the widget: the replica must be gone before the engine
    // prunes QML's property-cache entry for it (see LogosQmlBridge.h, module()).
    bridge->setParent(qmlWidget->engine());

    // Attach BEFORE setSource() below, which is where QML actually runs: an app
    // calling logos.request() from Component.onCompleted would otherwise be an
    // unknown bridge and get `unavailable` from its own shell.
    // The single funnel for every ui_qml app — the QML-only and backend paths
    // both converge here.
    if (m_intentAdapter)
        m_intentAdapter->attach(request.name, bridge);

    qmlWidget->rootContext()->setContextProperty("logos", bridge);
    qmlWidget->rootContext()->setContextProperty("isActiveTab", true);
    qmlWidget->setSource(QUrl::fromLocalFile(request.qmlViewPath));

    if (!request.iconPath.isEmpty())
        qmlWidget->setWindowIcon(QIcon(request.iconPath));

    if (qmlWidget->status() == QQuickWidget::Error) {
        qWarning() << "Failed to load ui_qml view" << request.name;
        const auto errors = qmlWidget->errors();
        for (const QQmlError& error : errors) qWarning() << error.toString();
        qmlWidget->deleteLater();
        if (viewHost) { viewHost->stop(); delete viewHost; }
        setLoading(request.name, false);
        emit pluginLoadFailed(request.name,
            QStringLiteral("Failed to load QML view for ") + request.name);
        return;
    }

    // The backend went ready before this QML existed (see onHostReady), so
    // tell the view now; views wait for viewModuleReadyChanged to show content.
    bridge->replayViewModuleState();

    setLoading(request.name, false);
    emit pluginLoaded(request.name, qmlWidget, nullptr, UIPluginType::UiQml, viewHost);
}
