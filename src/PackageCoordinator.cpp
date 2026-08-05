#include "PackageCoordinator.h"
#include "InstallRegistry.h"
#include "AppsFilterProxy.h"
#include "AppsModel.h"
#include "CoreModuleManager.h"
#include "UIPluginManager.h"
#include "LogosBasecampPaths.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPointer>
#include <QScopeGuard>
#include <QTimer>

#include <functional>
#include <memory>
#include <utility>

#include "logos_sdk.h"

PackageCoordinator::PackageCoordinator(LogosAPI* logosAPI,
                               CoreModuleManager* coreModuleManager,
                               UIPluginManager* uiPluginManager,
                               AppsModel* appsModel,
                               QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
    , m_coreModuleManager(coreModuleManager)
    , m_uiPluginManager(uiPluginManager)
    , m_appsModel(appsModel)
    , m_installRegistry(new InstallRegistry(this))
{
    m_pmEventsWired = subscribeToPackageInstallationEvents();
    m_pdEventsWired = subscribeToPackageDownloaderEvents();

    // Re-wire (or retry) the IPC wiring when package modules leave/rejoin
    // the loaded set.
    if (m_coreModuleManager) {
        connect(m_coreModuleManager, &CoreModuleManager::coreModulesChanged,
                this, &PackageCoordinator::onCoreModuleSetChanged);
    }

    // NB: initial metadata fetch is deferred until MainUIBackend calls
    // refresh() — the uiPluginsFetched signal would otherwise fire before
    // UIPluginManager's setPackageCoordinator runs and the slot connection
    // lands, causing the first-paint UI-plugin list to be empty until the
    // next file-install event triggers a re-scan.
}

PackageCoordinator::~PackageCoordinator() = default;

bool PackageCoordinator::subscribeToPackageInstallationEvents()
{
    if (!m_logosAPI) {
        return false;
    }

    LogosAPIClient* client = m_logosAPI->getClient("package_manager");
    if (!client || !client->isConnected()) {
        return false;
    }

    LogosModules logos(m_logosAPI);

    bool ok = true;

    // Subscribe once per event per live wiring (nothing can unsubscribe, so a
    // retry must not re-issue handlers that already landed); the first miss
    // clears `ok` and skips the rest — each one blocks ~20s.
    auto subscribe = [this, &logos, &ok](const char* event,
                                         std::function<void(const QVariantList&)> handler) {
        const QString name = QString::fromLatin1(event);
        if (!ok || m_pmSubscribedEvents.contains(name)) return;
        if (logos.package_manager.on(name, std::move(handler))) {
            m_pmSubscribedEvents.insert(name);
            return;
        }
        qWarning() << "PackageCoordinator: failed to subscribe to package_manager"
                      " event" << event;
        ok = false;
    };

    // First subscription doubles as the reachability probe — isConnected()
    // can't see a not-yet-acquirable replica (normal for a beat after a
    // (re)load), so bail on the first miss instead of blocking per call.
    subscribe("corePluginFileInstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "Core module file installed:" << data[0].toString();
        QTimer::singleShot(100, this, [this]() {
            if (m_coreModuleManager) m_coreModuleManager->refresh();
            // Also rescan UI plugin metadata — a newly installed core module
            // could satisfy a dependency that previously left a UI module
            // marked with missing deps, so the sidebar red-cross needs to clear.
            fetchUiPluginMetadata();
        });
    });
    if (!ok) {
        qWarning() << "PackageCoordinator: package_manager is loaded but its event"
                      " replica is not acquirable yet — leaving IPC unwired,"
                      " the core-module-set watcher will retry";
        return false;
    }

    // Configure the package_manager module's install directories. These can
    // fail independently of the event replica, so report through CallError.
    auto configureDirectory = [&ok](const char* what,
                                    const std::function<void(logos::CallError*)>& call) {
        if (!ok) return;
        logos::CallError err;
        call(&err);
        if (err.ok()) return;
        qWarning() << "PackageCoordinator: failed to configure package_manager"
                   << what << "—" << QString::fromStdString(err.message);
        ok = false;
    };

    configureDirectory("embedded modules directory", [&](logos::CallError* err) {
        logos.package_manager.setEmbeddedModulesDirectory(
            LogosBasecampPaths::embeddedModulesDirectory(), err);
    });
    configureDirectory("user modules directory", [&](logos::CallError* err) {
        logos.package_manager.setUserModulesDirectory(
            LogosBasecampPaths::modulesDirectory(), err);
    });
    configureDirectory("embedded UI plugins directory", [&](logos::CallError* err) {
        logos.package_manager.setEmbeddedUiPluginsDirectory(
            LogosBasecampPaths::embeddedPluginsDirectory(), err);
    });
    configureDirectory("user UI plugins directory", [&](logos::CallError* err) {
        logos.package_manager.setUserUiPluginsDirectory(
            LogosBasecampPaths::pluginsDirectory(), err);
    });

    subscribe("uiPluginFileInstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "UI plugin file installed:" << data[0].toString();
        QTimer::singleShot(100, this, [this]() {
            fetchUiPluginMetadata();
        });
    });

    // Uninstall events — mirror the install handlers. We rescan UI plugin
    // metadata in both cases because a core uninstall can make a previously
    // satisfied UI dep go missing, and a UI uninstall flat-out removes the
    // plugin from UIPluginManager's metadata. The 100ms settle matches
    // install to absorb rapid batched events.
    subscribe("corePluginUninstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "Core module uninstalled:" << data[0].toString();
        QTimer::singleShot(100, this, [this]() {
            if (m_coreModuleManager) m_coreModuleManager->refresh();
            fetchUiPluginMetadata();
        });
    });

    subscribe("uiPluginUninstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "UI plugin uninstalled:" << data[0].toString();
        QTimer::singleShot(100, this, [this]() {
            fetchUiPluginMetadata();
        });
    });

    // Clear any pending action left over from a prior session that crashed
    // mid-dialog. The module retains m_pendingAction across Basecamp restarts
    // (it's non-persistent but survives our process death since it lives in
    // package_manager's process); without this reset, the first request after
    // a crash would get rejected with "another X is in progress".
    logos.package_manager.resetPendingActionAsync([](QVariantMap){});

    // Gated uninstall/upgrade events. package_manager emits these BEFORE any
    // destructive work, with a 3s ack timer running; onBeforeUninstall /
    // onBeforeUpgrade acks synchronously and — if the ack landed in time —
    // drives the cascade confirmation dialog. See PackageCoordinator.h for the
    // ack-gated protocol rationale.
    subscribe("beforeUninstall", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        const QByteArray payload = data.first().toString().toUtf8();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "beforeUninstall payload parse error:" << err.errorString();
            return;
        }
        const QJsonObject obj = doc.object();
        const QString name = obj.value("name").toString();
        QStringList installedDeps;
        for (const QJsonValue& v : obj.value("installedDependents").toArray()) {
            if (v.isString()) installedDeps.append(v.toString());
        }
        onBeforeUninstall(name, installedDeps);
    });

    subscribe("beforeUpgrade", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        const QByteArray payload = data.first().toString().toUtf8();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "beforeUpgrade payload parse error:" << err.errorString();
            return;
        }
        const QJsonObject obj = doc.object();
        const QString name       = obj.value("name").toString();
        const QString releaseTag = obj.value("releaseTag").toString();
        const int     mode       = obj.value("mode").toInt();
        QStringList installedDeps;
        for (const QJsonValue& v : obj.value("installedDependents").toArray()) {
            if (v.isString()) installedDeps.append(v.toString());
        }
        // Transitive dep changes the initiator (PMU) resolved for this swap —
        // opaque display data the dialog lists. Absent/empty on a bare upgrade.
        const QVariantList depChanges = obj.value("depChanges").toArray().toVariantList();
        onBeforeUpgrade(name, releaseTag, mode, installedDeps, depChanges);
    });

    // beforeInstall — the catalog-install gate. Same ack-then-dialog shape as
    // beforeUpgrade, but with no dependents (a fresh install unloads nothing).
    subscribe("beforeInstall", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        const QByteArray payload = data.first().toString().toUtf8();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "beforeInstall payload parse error:" << err.errorString();
            return;
        }
        const QJsonObject obj = doc.object();
        const QString name       = obj.value("name").toString();
        const QString releaseTag = obj.value("releaseTag").toString();
        const QVariantList depChanges = obj.value("depChanges").toArray().toVariantList();
        onBeforeInstall(name, releaseTag, depChanges);
    });

    // Multi-uninstall is a separate event so existing single-uninstall handlers
    // don't have to peek at the payload shape to disambiguate.
    subscribe("beforeMultiUninstall", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        const QByteArray payload = data.first().toString().toUtf8();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "beforeMultiUninstall payload parse error:" << err.errorString();
            return;
        }
        const QJsonObject obj = doc.object();
        QStringList names;
        for (const QJsonValue& v : obj.value("names").toArray()) {
            if (v.isString()) names.append(v.toString());
        }
        QStringList installedDeps;
        for (const QJsonValue& v : obj.value("installedDependents").toArray()) {
            if (v.isString()) installedDeps.append(v.toString());
        }
        onBeforeMultiUninstall(names, installedDeps);
    });

    // The multi-uninstall cancellation counterpart. package_manager_ui
    // subscribes to uninstallCancelled and toasts it, so the single-uninstall
    // path is already covered on that surface — but nothing anywhere listens
    // for the multi variant, and the App Manager now runs exclusively through
    // it. Without this a 3s ack timeout (or a cancel that raced the dialog)
    // is completely silent.
    logos.package_manager.on("multiUninstallCancelled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        const QJsonDocument doc =
            QJsonDocument::fromJson(data.first().toString().toUtf8());
        if (!doc.isObject()) return;
        const QString reason = doc.object().value("reason").toString();
        // A user-driven cancel already had a visible dialog; only surface the
        // ones the user didn't ask for.
        m_lastRequestedTargets.clear();
        if (reason.contains(QStringLiteral("user cancelled"))) return;
        qWarning() << "multiUninstallCancelled:" << reason;
    });
}

void PackageCoordinator::onCoreModuleSetChanged()
{
    if (!m_coreModuleManager) return;
    const QStringList loaded = m_coreModuleManager->loadedModules();
    const bool pmLoaded = loaded.contains(QStringLiteral("package_manager"));
    const bool pdLoaded = loaded.contains(QStringLiteral("package_downloader"));

    // Module gone: replica and subscriptions are dead — re-wire on next sighting.
    if (!pmLoaded) {
        m_pmEventsWired = false;
        m_pmSubscribedEvents.clear();
    }
    if (!pdLoaded) m_pdEventsWired = false;

    const bool needRewire = (pmLoaded && !m_pmEventsWired)
                         || (pdLoaded && !m_pdEventsWired);
    if (!needRewire || m_rewireQueued) return;

    // Queued: the rewire does synchronous IPC, keep it off this signal stack.
    m_rewireQueued = true;
    QPointer<PackageCoordinator> self(this);
    QTimer::singleShot(0, this, [self]() {
        if (!self) return;
        self->m_rewireQueued = false;
        self->rewirePackageIpc();
    });
}

void PackageCoordinator::armRewireRetry()
{
    if (m_rewireQueued) return;
    m_rewireQueued = true;
    QPointer<PackageCoordinator> self(this);
    QTimer::singleShot(kRewireRetryMs, this, [self]() {
        if (!self) return;
        self->m_rewireQueued = false;
        self->rewirePackageIpc();
    });
}

void PackageCoordinator::rewirePackageIpc()
{
    if (!m_logosAPI || !m_coreModuleManager) return;

    // Everything below blocks on IPC and spins nested event loops, which
    // dispatch the very timers that scheduled us. Never run inside a plugin
    // load or core module op — our blocking acquire would starve the outer
    // handshake until it times out. Deferring is safe: the 2s stats tick
    // re-fires coreModulesChanged anyway.
    const bool uiLoadBusy = m_uiPluginManager
                         && m_uiPluginManager->uiPluginLoadInFlight();
    const bool coreOpBusy = m_coreModuleManager
                         && m_coreModuleManager->moduleOperationInFlight();
    if (uiLoadBusy || coreOpBusy) {
        qDebug() << "PackageCoordinator: deferring IPC re-wire —"
                 << (uiLoadBusy ? "a UI-plugin load" : "a core module load/unload")
                 << "is in flight";
        armRewireRetry();
        return;
    }

    // Don't re-enter ourselves; re-arm so a failing pass still gets retried.
    if (m_rewireRunning) {
        armRewireRetry();
        return;
    }
    m_rewireRunning = true;
    // qScopeGuard: the SDK wrappers can throw.
    const auto clearRunning = qScopeGuard([this] { m_rewireRunning = false; });

    const QStringList loaded = m_coreModuleManager->loadedModules();
    bool rewired = false;

    // Reconnect drops the client's stale replica handles (RemoteLogosObject
    // never reports itself stale), then re-run the event subscriptions.
    if (!m_pmEventsWired && loaded.contains(QStringLiteral("package_manager"))) {
        if (LogosAPIClient* client = m_logosAPI->getClient("package_manager")) {
            // The old handlers die with the node — re-issue every event.
            m_pmSubscribedEvents.clear();
            client->reconnect();
        }
        m_pmEventsWired = subscribeToPackageInstallationEvents();
        if (m_pmEventsWired) {
            qDebug() << "PackageCoordinator: re-wired package_manager IPC after module (re)load";
            rewired = true;
        }
    }

    if (!m_pdEventsWired && loaded.contains(QStringLiteral("package_downloader"))) {
        if (LogosAPIClient* client = m_logosAPI->getClient("package_downloader"))
            client->reconnect();
        m_pdEventsWired = subscribeToPackageDownloaderEvents();
        if (m_pdEventsWired) {
            qDebug() << "PackageCoordinator: re-wired package_downloader IPC after module (re)load";
            rewired = true;
        }
    }

    if (rewired) {
        // Repopulate everything fetched over the old wiring.
        refresh();
    }
}

bool PackageCoordinator::subscribeToPackageDownloaderEvents()
{
    if (!m_logosAPI) return false;

    LogosAPIClient* client = m_logosAPI->getClient("package_downloader");
    if (!client || !client->isConnected()) return false;

    // Same contract as the package_manager twin: report whether the
    // subscription actually landed.
    LogosModules logos(m_logosAPI);
    const bool ok = logos.package_downloader.on("catalogChanged",
                                                [this](const QVariantList&) {
        refreshRepositories();
        refresh();
    });
    if (!ok) {
        qWarning() << "PackageCoordinator: package_downloader is loaded but its"
                      " event replica is not acquirable yet — leaving IPC"
                      " unwired, the core-module-set watcher will retry";
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Read-only accessors over the caches.
// ---------------------------------------------------------------------------

QString PackageCoordinator::installType(const QString& name) const
{
    return m_installTypeByModule.value(name);
}

QStringList PackageCoordinator::missingDepsOf(const QString& name) const
{
    return m_missingDepsByModule.value(name);
}

QStringList PackageCoordinator::dependentsOf(const QString& name) const
{
    return m_dependentsByModule.value(name);
}

QString PackageCoordinator::displayNameFor(const QString& name) const
{
    const QString dn = m_displayNameByModule.value(name);
    if (!dn.isEmpty()) return dn;
    return name;
}

QString PackageCoordinator::colorFor(const QString& name) const
{
    if (!m_appsModel) return {};
    return m_appsModel->rowDataByName(name).value("color").toString();
}

// ---------------------------------------------------------------------------
// Gated uninstall — entry points
// ---------------------------------------------------------------------------

void PackageCoordinator::uninstallUiModule(const QString& moduleName)
{
    qDebug() << "uninstallUiModule:" << moduleName;

    // Main UI is protected — uninstalling it would brick Basecamp. Every
    // other "is this uninstallable?" check (embedded-refusal, unknown-module)
    // now lives in the package_manager module itself, so there's no duplicate
    // gating here. This guard stays local because "don't kill your own UI"
    // is a Basecamp concern, not a package-lifecycle concern.
    if (moduleName == QStringLiteral("main_ui")) {
        qWarning() << "Refusing to uninstall main_ui";
        return;
    }

    // Kick off the gated request. The module:
    //   1. Sets its pending slot, emits "beforeUninstall" with the installed-
    //      dependents list, and starts the 3s ack timer.
    //   2. We catch the event in onBeforeUninstall, ack, and show the cascade
    //      dialog. Reentry protection lives in the module (global single-slot
    //      pending) so a concurrent second click gets rejected synchronously.
    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.requestUninstallAsync(
        moduleName, [self, moduleName](QVariantMap result) {
            if (!self) return;
            if (result.value("success", false).toBool()) return;
            const QString error = result.value("error").toString();
            qWarning() << "requestUninstall rejected for" << moduleName << ":" << error;
        });
}

void PackageCoordinator::uninstallApp(const QString& name, const QString& repositoryUrl)
{
    Q_UNUSED(repositoryUrl);
    if (name.isEmpty()) return;
    if (name == QStringLiteral("main_ui")) {
        qWarning() << "Refusing to uninstall main_ui";
        return;
    }

    if (m_dependencyDataReady) {
        performUninstallApp(name);
        return;
    }

    // Cache still populating — open the dialog in a loading state and defer
    // the real dispatch until dependencyDataReadyChanged fires. Drop any
    // prior deferred connection first so re-clicking during the cold-boot
    // window doesn't stack callbacks.
    QObject::disconnect(m_pendingUninstallAppConn);
    m_pendingUninstallAppName = name;
    emit uninstallPlanRequested(
        buildLoadingPayload(name, QStringLiteral("app")));

    QPointer<PackageCoordinator> self(this);
    m_pendingUninstallAppConn = connect(
        this, &PackageCoordinator::dependencyDataReadyChanged, this,
        [self, name]() {
            if (!self) return;
            if (self->m_pendingUninstallAppName != name) return;   // cancelled
            self->m_pendingUninstallAppName.clear();
            QObject::disconnect(self->m_pendingUninstallAppConn);
            self->performUninstallApp(name);
        });
}

void PackageCoordinator::performUninstallApp(const QString& name)
{
    // Compose: target + orphaned deps. requestMultiUninstall refuses the
    // whole batch if any member is embedded, so filter here.
    const uninstallplan::Plan plan =
        uninstallplan::composeFrom(planInput({name}));
    if (plan.batch.isEmpty()) {
        qWarning() << "performUninstallApp:" << name << "is not installed — ignoring";
        return;
    }

    qDebug() << "performUninstallApp:" << name << "batch=" << plan.batch;

    // Remembered only so the explain pass can flag which rows the user
    // actually picked; the batch itself is recomputed on arrival.
    m_lastRequestedTargets = {name};
    m_lastRequestKind      = QStringLiteral("app");

    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    const QStringList batch = plan.batch;
    logos.package_manager.requestMultiUninstallAsync(
        batch, [self, batch](QVariantMap result) {
            if (!self) return;
            if (result.value("success", false).toBool()) return;
            const QString error = result.value("error").toString();
            qWarning() << "requestMultiUninstall rejected for" << batch << ":" << error;
            self->m_lastRequestedTargets.clear();
        });
}

void PackageCoordinator::cancelPendingUninstallApp(const QString& name)
{
    if (m_pendingUninstallAppName != name) return;
    m_pendingUninstallAppName.clear();
    QObject::disconnect(m_pendingUninstallAppConn);
}

void PackageCoordinator::uninstallCoreModule(const QString& moduleName)
{
    // Same flow as uninstallUiModule — requestUninstall is type-agnostic.
    // The module's pending state is global so there's no type-specific
    // bookkeeping to do here.
    qDebug() << "uninstallCoreModule:" << moduleName;

    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.requestUninstallAsync(
        moduleName, [self, moduleName](QVariantMap result) {
            if (!self) return;
            if (result.value("success", false).toBool()) return;
            const QString error = result.value("error").toString();
            qWarning() << "requestUninstall rejected for" << moduleName << ":" << error;
        });
}

// ---------------------------------------------------------------------------
// Uninstall plan — compose (what to remove) and explain (why the rest stays)
// ---------------------------------------------------------------------------

QSet<QString> PackageCoordinator::loadedNames() const
{
    QSet<QString> loaded;
    if (m_coreModuleManager) {
        const QStringList core = m_coreModuleManager->loadedModules();
        loaded = QSet<QString>(core.cbegin(), core.cend());
    }
    if (m_uiPluginManager) {
        // intersectWithLoaded already merges core + in-process UI widgets, so
        // asking it about every installed name yields the full loaded set —
        // including ui_qml plugins, which never appear in loadedModules().
        const QStringList all = m_installTypeByModule.keys();
        for (const QString& n : m_uiPluginManager->intersectWithLoaded(all))
            loaded.insert(n);
    }
    return loaded;
}

uninstallplan::Input PackageCoordinator::planInput(const QStringList& targets) const
{
    uninstallplan::Input in;
    in.targets = targets;
    for (auto it = m_dependenciesByModule.cbegin();
         it != m_dependenciesByModule.cend(); ++it) {
        in.dependencies.insert(it.key(), it.value());
    }
    for (auto it = m_dependentsByModule.cbegin();
         it != m_dependentsByModule.cend(); ++it) {
        in.dependents.insert(it.key(), it.value());
    }
    // m_installTypeByModule is keyed on every installed package (UI + core),
    // which makes its key list the installed set — and its values the
    // embedded/user split the plan needs.
    for (auto it = m_installTypeByModule.cbegin();
         it != m_installTypeByModule.cend(); ++it) {
        in.installed << it.key();
        if (it.value() == QLatin1String("embedded")) in.embedded.insert(it.key());
    }
    in.protectedNames = {QStringLiteral("main_ui")};
    for (auto it = m_displayNameByModule.cbegin();
         it != m_displayNameByModule.cend(); ++it) {
        in.displayNames.insert(it.key(), it.value());
    }
    in.versions = m_installedVersionByName;
    in.loaded   = loadedNames();
    return in;
}

namespace {

QVariantMap removableRowToMap(const uninstallplan::Row& r)
{
    return {
        {QStringLiteral("name"),        r.name},
        {QStringLiteral("displayName"), r.displayName},
        {QStringLiteral("version"),     r.version},
        {QStringLiteral("isTarget"),    r.isTarget},
        {QStringLiteral("isLoaded"),    r.isLoaded},
    };
}

}  // namespace

QVariantMap PackageCoordinator::buildLoadingPayload(const QString& name,
                                                    const QString& kind) const
{
    // Stub payload; UninstallDialog reads `loading` to swap in the spinner.
    // `multi: false` sends cancel through cancelPendingUninstallApp (no
    // module IPC to unwind).
    QVariantMap payload;
    payload.insert(QStringLiteral("loading"),    true);
    payload.insert(QStringLiteral("kind"),       kind);
    payload.insert(QStringLiteral("multi"),      false);
    payload.insert(QStringLiteral("batch"),      QVariantList{name});
    payload.insert(QStringLiteral("targetName"), name);
    payload.insert(QStringLiteral("removable"),  QVariantList{});
    payload.insert(QStringLiteral("kept"),       QVariantList{});
    payload.insert(QStringLiteral("dependents"), QVariantList{});
    return payload;
}

QVariantMap PackageCoordinator::buildPlanPayload(const QStringList& batch,
                                                 const QStringList& installedDependents,
                                                 const QString& kind,
                                                 bool multi) const
{
    // Explain pass: the batch is already fixed, so it IS the target list and
    // no orphan expansion runs. Everything left in the closure lands in
    // `kept` with the reason it survived.
    const uninstallplan::Plan plan =
        uninstallplan::explainOf(planInput(batch));

    // Which of those rows the user actually picked. Empty (PMUI / Settings)
    // means "all of them", which is the honest reading there.
    const QSet<QString> picked(m_lastRequestedTargets.cbegin(),
                               m_lastRequestedTargets.cend());

    QVariantList removable;
    removable.reserve(plan.removable.size());
    for (const uninstallplan::Row& r : plan.removable) {
        uninstallplan::Row row = r;
        row.isTarget = picked.isEmpty() || picked.contains(r.name);
        removable.append(removableRowToMap(row));
    }

    QVariantList kept;
    kept.reserve(plan.kept.size());
    for (const uninstallplan::KeptRow& k : plan.kept) {
        kept.append(QVariantMap{
            {QStringLiteral("name"),        k.name},
            {QStringLiteral("displayName"), k.displayName},
            // Enum → string for the QML payload. See uninstallplan::reasonName.
            {QStringLiteral("reason"),      uninstallplan::reasonName(k.reason)},
            {QStringLiteral("requiredBy"),  k.requiredBy},
        });
    }

    // The module's dependents list is authoritative (it walked the same graph
    // at request time, under its own lock). Fall back to the locally computed
    // one only when the gate didn't carry it.
    QStringList dependentNames = installedDependents;
    if (dependentNames.isEmpty()) {
        for (const uninstallplan::Row& r : plan.dependents) dependentNames << r.name;
    }
    const QSet<QString> loaded    = loadedNames();
    const QSet<QString> batchSet(batch.cbegin(), batch.cend());
    QVariantList dependents;
    for (const QString& n : dependentNames) {
        if (n.isEmpty() || batchSet.contains(n)) continue;
        dependents.append(QVariantMap{
            {QStringLiteral("name"),        n},
            {QStringLiteral("displayName"), displayNameFor(n)},
            {QStringLiteral("isLoaded"),    loaded.contains(n)},
        });
    }

    return {
        {QStringLiteral("kind"),       kind},
        {QStringLiteral("multi"),      multi},
        {QStringLiteral("batch"),      batch},
        {QStringLiteral("removable"),  removable},
        {QStringLiteral("kept"),       kept},
        {QStringLiteral("dependents"), dependents},
    };
}

// ---------------------------------------------------------------------------
// Cascade confirmation — triggered from QML once the user OKs the dialog.
// ---------------------------------------------------------------------------

void PackageCoordinator::confirmUninstallCascade(const QString& moduleName)
{
    if ((m_pendingAction.op != PendingOp::UninstallCascade &&
         m_pendingAction.op != PendingOp::UpgradeCascade)
        || m_pendingAction.name != moduleName) {
        qWarning() << "confirmUninstallCascade for" << moduleName
                   << "but pending action is" << m_pendingAction.name;
        return;
    }

    // Snapshot before clearing — the callbacks below capture by value.
    const bool    isUpgrade         = (m_pendingAction.op == PendingOp::UpgradeCascade);
    const QString releaseTag        = m_pendingAction.releaseTag;
    m_pendingAction = {};

    // Defer the cascade body off the QML click stack: unloadModuleWithDependents
    // spins a nested event loop, and running that under the dialog's onClicked
    // handler trips a QQmlData::destroyed qFatal. Pending state is cleared above;
    // queue the rest so the click handler unwinds first.
    QPointer<PackageCoordinator> selfDefer(this);
    QMetaObject::invokeMethod(this,
        [this, selfDefer, moduleName, isUpgrade, releaseTag]() {
        if (!selfDefer) return;

    // Snapshot the loaded-dependents list BEFORE the cascade — once
    // unloadModuleWithDependents returns, the target is off the loaded-
    // modules list and the filter would come up empty. UI-plugin dependents
    // need teardown in-process because the core cascade only kills core
    // modules (QProcess termination). Without this pass, e.g. accounts_ui
    // stays wired to a now-dead accounts_module.
    QStringList loadedDeps;
    if (m_uiPluginManager) {
        loadedDeps = m_uiPluginManager->intersectWithLoaded(
            m_dependentsByModule.value(moduleName));
    }

    const QStringList loadedCore = m_coreModuleManager
        ? m_coreModuleManager->loadedModules()
        : QStringList{};

    // Core cascade: terminate the target process (if it's a loaded core
    // module) plus any loaded core-module dependents. Local-mode / pure-UI
    // targets aren't in loadedModules and the function will return
    // non-zero; we tolerate that and proceed to the UI teardown and module-
    // side confirm call — the user-visible action (deleting the package /
    // swapping versions) is what we must preserve.
    if (loadedCore.contains(moduleName) || !loadedDeps.isEmpty()) {
        qDebug() << "Cascade-unloading before uninstall:" << moduleName;
        bool ok = m_coreModuleManager
                ? m_coreModuleManager->unloadModuleWithDependents(moduleName)
                : false;
        if (!ok) {
            qWarning() << "Cascade unload failed during uninstall of" << moduleName
                       << "— proceeding with confirm anyway";
        }
    }

    // UI plugins are in-process widgets managed by UIPluginManager, not core
    // processes. teardownUiPluginWidget is idempotent, so calling it for
    // names that aren't loaded UI plugins is harmless.
    if (m_uiPluginManager) {
        for (const QString& dep : loadedDeps) {
            m_uiPluginManager->teardownUiPluginWidget(dep);
        }
        m_uiPluginManager->teardownUiPluginWidget(moduleName);
    }

    // Hand the actual package-lifecycle work back to the module.
    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    if (isUpgrade) {
        // Upgrade — the module does the uninstall step + emits
        // upgradeUninstallDone for PMU to drive the install of the new
        // version (a catalog download, or the local .lgx it stashed).
        logos.package_manager.confirmUpgradeAsync(moduleName, releaseTag,
            [moduleName](QVariantMap r) {
                if (!r.value("success", false).toBool()) {
                    qWarning() << "confirmUpgrade rejected for" << moduleName
                               << ":" << r.value("error").toString();
                }
            });
    } else {
        // Plain uninstall.
        logos.package_manager.confirmUninstallAsync(moduleName,
            [moduleName](QVariantMap r) {
                if (!r.value("success", false).toBool()) {
                    qWarning() << "confirmUninstall rejected for" << moduleName
                               << ":" << r.value("error").toString();
                }
            });
    }

    emit coreModulesChanged();
    emit uiModulesChanged();
    emit launcherAppsChanged();
    }, Qt::QueuedConnection);  // run the cascade off the click stack
}

void PackageCoordinator::refresh()
{
    fetchUiPluginMetadata();
    refreshRepositories();
}

void PackageCoordinator::remoteRefresh()
{
    if (!m_appsLoading) {
        m_appsLoading = true;
        emit appsLoadingChanged();
    }

    LogosAPIClient* dlClient = m_logosAPI
        ? m_logosAPI->getClient("package_downloader")
        : nullptr;
    if (!dlClient || !dlClient->isConnected()) {
        // Downloader unreachable — fall back to a local re-sync
        refresh();
        return;
    }

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_downloader.refreshCatalogAsync([self](QVariantMap r) {
        if (!self) return;
        const QString err = r.value(QStringLiteral("error")).toString();
        if (!err.isEmpty())
            qWarning() << "package_downloader.refreshCatalog reported:" << err;
        self->refresh();
    });
}

void PackageCoordinator::cancelPendingAction(const QString& moduleName)
{
    if (m_pendingAction.op == PendingOp::None || m_pendingAction.name != moduleName) {
        // MainUIBackend fans out cancelPendingAction to both managers so one
        // of them is always a no-op — don't even warn here.
        return;
    }
    qDebug() << "Cancelling pending package action for" << moduleName;
    const PendingOp op = m_pendingAction.op;
    const QString   releaseTag = m_pendingAction.releaseTag;
    m_pendingAction = {};

    // Uninstall / Upgrade are gated by the module — tell it we bailed;
    // otherwise its pending slot stays set and the next request is
    // rejected with "another <op> is in progress".
    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    if (op == PendingOp::UpgradeCascade) {
        logos.package_manager.cancelUpgradeAsync(moduleName, releaseTag,
            [](QVariantMap){});
    } else {
        logos.package_manager.cancelUninstallAsync(moduleName,
            [](QVariantMap){});
    }
}

// ---------------------------------------------------------------------------
// Ack-gated cascade event handlers
//
// package_manager emits beforeUninstall / beforeUpgrade BEFORE any destructive
// work, then starts a short (3s) ack timer. Our contract:
//
//   1. Call ackPendingActionAsync IMMEDIATELY — before any UI work — to
//      cancel the module's ack timer and claim the pending slot.
//   2. Only emit the cascade dialog if the ack SUCCEEDED. If it failed, the
//      module already cancelled the request (timer fired, or another listener
//      got there first); emitting the dialog would let the user "Continue"
//      into a dead request.
//
// Once we own the slot, the user has unlimited time to decide. confirm* /
// cancel* on the module ends the flow.
// ---------------------------------------------------------------------------

void PackageCoordinator::onBeforeUninstall(const QString& name, const QStringList& installedDeps)
{
    if (!m_logosAPI) return;

    // Last-line defence. The module now rejects empty names at requestUninstall
    // (and PMU + QML filter them too), so this branch shouldn't fire in
    // practice. Kept because an empty name here would open a cascade dialog
    // titled "Uninstall ''?" — the user-reported symptom — and because it's
    // cheaper than re-debugging it if a future caller bypasses the gate.
    if (name.isEmpty()) {
        qWarning() << "PackageCoordinator::onBeforeUninstall received empty name — ignoring";
        return;
    }

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.ackPendingActionAsync(name,
        [self, name, installedDeps](QVariantMap result) {
            if (!self) return;
            if (!result.value("success", false).toBool()) {
                // The module already rejected us (ack timer fired, or the
                // request was cancelled by another path). Do NOT show a
                // dialog — package_manager already emitted uninstallCancelled
                // to its listeners.
                qWarning() << "ackPendingAction rejected for" << name << ":"
                           << result.value("error").toString();
                return;
            }
            self->m_pendingAction = {PendingOp::UninstallCascade, name, QString{}, 0};
            // A batch of one. The single-uninstall gate is what
            // package_manager_ui's trash icon and Settings → Modules use, so
            // confirm/cancel route to the single slots — hence multi=false.
            // Everything else (the Kept section, the dependent warning) is
            // identical to the multi path.
            const QVariantMap payload = self->buildPlanPayload(
                {name}, installedDeps, QStringLiteral("packages"), /*multi=*/false);
            self->m_lastRequestedTargets.clear();
            emit self->uninstallPlanRequested(payload);
        });
}

void PackageCoordinator::onBeforeUpgrade(const QString& name, const QString& releaseTag,
                                     int mode, const QStringList& installedDeps,
                                     const QVariantList& depChanges)
{
    if (!m_logosAPI) return;

    // Mirror of onBeforeUninstall — see rationale there.
    if (name.isEmpty()) {
        qWarning() << "PackageCoordinator::onBeforeUpgrade received empty name — ignoring";
        return;
    }

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.ackPendingActionAsync(name,
        [self, name, releaseTag, mode, installedDeps, depChanges](QVariantMap result) {
            if (!self) return;
            if (!result.value("success", false).toBool()) {
                qWarning() << "ackPendingAction rejected for" << name << ":"
                           << result.value("error").toString();
                return;
            }
            const QStringList loadedDeps = self->m_uiPluginManager
                ? self->m_uiPluginManager->intersectWithLoaded(installedDeps)
                : QStringList{};
            self->m_pendingAction = {PendingOp::UpgradeCascade, name, releaseTag, mode, {}};
            // Distinct cascade signal for upgrade/downgrade/reinstall: same
            // dependent-impact lists as the uninstall variant (the
            // package_manager performs an uninstall step first), but the
            // dialog needs the target version + UpgradeMode so it can lead
            // with "Upgrade to vX.Y.Z" / "Downgrade to vX.Y.Z" /
            // "Reinstall vX.Y.Z" instead of bare "Uninstall and Unload
            // Dependents?" — which previously caused user confusion on
            // downgrades that looked like a pure uninstall.
            emit self->upgradeCascadeConfirmationRequested(
                name, releaseTag, mode, installedDeps, loadedDeps, depChanges);
        });
}

void PackageCoordinator::onBeforeInstall(const QString& name, const QString& releaseTag,
                                         const QVariantList& depChanges)
{
    if (!m_logosAPI) return;

    if (name.isEmpty()) {
        qWarning() << "PackageCoordinator::onBeforeInstall received empty name — ignoring";
        return;
    }

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.ackPendingActionAsync(name,
        [self, name, releaseTag, depChanges](QVariantMap result) {
            if (!self) return;
            if (!result.value("success", false).toBool()) {
                qWarning() << "ackPendingAction rejected for" << name << ":"
                           << result.value("error").toString();
                return;
            }
            // No pending-slot / cascade work — a fresh install unloads nothing.
            // The dialog's confirm/cancel forward straight to the module gate.
            emit self->installGateConfirmationRequested(name, releaseTag, depChanges);
        });
}

void PackageCoordinator::confirmInstallGate(const QString& name)
{
    if (!m_logosAPI || name.isEmpty()) return;
    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.confirmInstallAsync(name, [self, name](QVariantMap result) {
        if (!self) return;
        if (!result.value("success", false).toBool())
            qWarning() << "confirmInstallGate rejected for" << name << ":"
                       << result.value("error").toString();
    });
}

void PackageCoordinator::cancelInstallGate(const QString& name)
{
    if (!m_logosAPI || name.isEmpty()) return;
    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.cancelInstallAsync(name, [self, name](QVariantMap result) {
        if (!self) return;
        if (!result.value("success", false).toBool())
            qWarning() << "cancelInstallGate rejected for" << name << ":"
                       << result.value("error").toString();
    });
}

void PackageCoordinator::onBeforeMultiUninstall(const QStringList& names,
                                                const QStringList& installedDeps)
{
    if (!m_logosAPI) return;
    if (names.isEmpty()) {
        qWarning() << "PackageCoordinator::onBeforeMultiUninstall received empty name list — ignoring";
        return;
    }

    // Ack with any name from the batch — the module's ackPendingAction accepts
    // any member of the pending batch's names for MultiUninstall (single-op
    // ack still requires exact-match against m_pendingAction.name). Picking
    // names.first() is convention; one ack closes the 3s timer for the whole
    // batch.
    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    const QString ackName = names.first();
    logos.package_manager.ackPendingActionAsync(ackName,
        [self, names, installedDeps, ackName](QVariantMap result) {
            if (!self) return;
            if (!result.value("success", false).toBool()) {
                qWarning() << "ackPendingAction (multi) rejected for" << ackName << ":"
                           << result.value("error").toString();
                // The module already cancelled; drop the remembered targets so
                // they can't mislabel the next batch that comes through.
                self->m_lastRequestedTargets.clear();
                return;
            }
            self->m_pendingAction = {PendingOp::MultiUninstallCascade, QString{}, QString{}, 0, names};
            // `kind` is "app" only when WE composed this batch from a single
            // App Manager row; a batch we didn't originate (package_manager_ui
            // bulk selection) is always "packages", and a stale target list
            // that isn't part of the arriving batch is dropped rather than
            // used to mislabel someone else's request.
            QString kind = QStringLiteral("packages");
            const QSet<QString> batchSet(names.cbegin(), names.cend());
            bool targetsBelong = !self->m_lastRequestedTargets.isEmpty();
            for (const QString& t : self->m_lastRequestedTargets)
                if (!batchSet.contains(t)) targetsBelong = false;
            if (targetsBelong) kind = self->m_lastRequestKind;
            else               self->m_lastRequestedTargets.clear();

            const QVariantMap payload =
                self->buildPlanPayload(names, installedDeps, kind, /*multi=*/true);
            self->m_lastRequestedTargets.clear();
            emit self->uninstallPlanRequested(payload);
        });
}

void PackageCoordinator::confirmUninstallMultiCascade(const QStringList& moduleNames)
{
    if (m_pendingAction.op != PendingOp::MultiUninstallCascade
        || m_pendingAction.names != moduleNames) {
        qWarning() << "confirmUninstallMultiCascade: no matching pending action — ignoring";
        return;
    }
    m_pendingAction = {};

    // Mirror of confirmUninstallCascade's cascade-unload, looped over each
    // batch member. Snapshot loaded UI dependents per name BEFORE the core
    // unload so the UI teardown pass can find them. Per-name dependents are
    // already deduped within each list, but a single dep could appear under
    // multiple targets — that's harmless because both teardownUiPluginWidget
    // and unloadModuleWithDependents are idempotent on already-gone modules.
    QStringList loadedCore = m_coreModuleManager
        ? m_coreModuleManager->loadedModules()
        : QStringList{};

    for (const QString& moduleName : moduleNames) {
        QStringList loadedDeps;
        if (m_uiPluginManager) {
            loadedDeps = m_uiPluginManager->intersectWithLoaded(
                m_dependentsByModule.value(moduleName));
        }
        if (loadedCore.contains(moduleName) || !loadedDeps.isEmpty()) {
            qDebug() << "Cascade-unloading before multi-uninstall:" << moduleName;
            bool ok = m_coreModuleManager
                    ? m_coreModuleManager->unloadModuleWithDependents(moduleName)
                    : false;
            if (!ok) {
                qWarning() << "Cascade unload failed during multi-uninstall of" << moduleName
                           << "— proceeding";
            }
            // Refresh the loaded snapshot so the next iteration sees the
            // post-cascade state (dependents that were also batch members
            // are already gone after their own cascade ran).
            loadedCore = m_coreModuleManager
                ? m_coreModuleManager->loadedModules()
                : QStringList{};
        }
        if (m_uiPluginManager) {
            for (const QString& dep : loadedDeps) {
                m_uiPluginManager->teardownUiPluginWidget(dep);
            }
            m_uiPluginManager->teardownUiPluginWidget(moduleName);
        }
    }

    // Hand the destructive work back to the module under one confirm call.
    // No rollback path on rejection: at this point the cascade-unload above
    // has already run, so a `success: false` here means the modules are
    // unloaded but their packages remain on disk. Rejection is rare in
    // normal flow (we just acked, the module's pending state is ours) — most
    // commonly it'd indicate a name-list mismatch we should never construct.
    // The user can re-load via the Modules tab if they hit this.
    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    logos.package_manager.confirmMultiUninstallAsync(moduleNames,
        [moduleNames](QVariantMap r) {
            if (!r.value("success", false).toBool()) {
                qWarning() << "confirmMultiUninstall rejected:"
                           << r.value("error").toString();
            }
        });

    emit coreModulesChanged();
    emit uiModulesChanged();
    emit launcherAppsChanged();
}

void PackageCoordinator::cancelMultiUninstall(const QStringList& moduleNames)
{
    if (m_pendingAction.op != PendingOp::MultiUninstallCascade
        || m_pendingAction.names != moduleNames) {
        // Cancel was fanned out from MainUIBackend or arrived after another
        // path already cleared the slot — treat as no-op rather than warning.
        return;
    }
    m_pendingAction = {};
    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    logos.package_manager.cancelMultiUninstallAsync(moduleNames,
        [](QVariantMap){});
}

// ---------------------------------------------------------------------------
// Metadata refresh chain
// ---------------------------------------------------------------------------

void PackageCoordinator::fetchUiPluginMetadata()
{
    if (!m_logosAPI) {
        if (m_appsLoading) {
            m_appsLoading = false;
            emit appsLoadingChanged();
        }
        emit uiModulesChanged();
        return;
    }

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.getInstalledUiPluginsAsync([self](QVariantList uiPlugins) {
        if (!self) return;

        if (uiPlugins.isEmpty()) {
            // A truthful reply always contains at least main_ui — empty means
            // the fetch failed or raced a restart; keep last-known metadata.
            qWarning() << "PackageCoordinator: getInstalledUiPlugins returned"
                          " an empty list — treating as a failed fetch,"
                          " keeping last-known UI-plugin metadata";
            if (self->m_appsLoading) {
                self->m_appsLoading = false;
                emit self->appsLoadingChanged();
            }
            return;
        }

        // Seed installType for the UI-plugin subset. refreshDependencyInfo's
        // full-scan pass will overwrite this with the core-inclusive version;
        // we do this first so QML has a non-empty map to key on during the
        // window between the two async calls.
        self->m_installTypeByModule.clear();
        QHash<QString, QString> installedByName;  // name → installed version
        for (const QVariant& item : uiPlugins) {
            QVariantMap pluginInfo = item.toMap();
            const QString name = pluginInfo.value("name").toString();
            if (name.isEmpty()) continue;
            self->m_installTypeByModule[name] = pluginInfo.value("installType").toString();
            const QString version = pluginInfo.value("version").toString();
            installedByName.insert(name, version);
            const QString rootHash =
                pluginInfo.value("hashes").toMap().value("root").toString();
            self->m_installedHashByName.insert(name, rootHash);
        }

        // Push the raw UI-plugin list to UIPluginManager — that's where the
        // UI-specific cache (used for widget loading) lives.
        emit self->uiPluginsFetched(uiPlugins);
        emit self->uiModulesChanged();
        emit self->launcherAppsChanged();
        self->refreshDependencyInfo();

        // Kick off the App-Manager catalog fetch.
        self->tryFetchCatalog(installedByName, /*retriesLeft=*/10);
    });
}

void PackageCoordinator::tryFetchCatalog(const QHash<QString, QString>& installedByName, int retriesLeft)
{
    LogosAPIClient* dlClient = m_logosAPI
        ? m_logosAPI->getClient("package_downloader")
        : nullptr;

    if (dlClient && dlClient->isConnected()) {
        QPointer<PackageCoordinator> self(this);
        dlClient->invokeRemoteMethodAsync(
            "package_downloader", "getCatalog", QVariantList{},
            [self, installedByName](QVariant catalogVar) {
                if (!self) return;
                const QVariantList catalog = catalogVar.toList();
                self->buildCatalogIndexes(catalog);
                self->populateAppsModel(catalog, installedByName);
            });
        return;
    }

    if (retriesLeft <= 0) {
        // Give up — drop the App Manager overlay so Reload doesn't stick.
        if (m_appsLoading) {
            m_appsLoading = false;
            emit appsLoadingChanged();
        }
        return;
    }

    QPointer<PackageCoordinator> self(this);
    QTimer::singleShot(200, this, [self, installedByName, retriesLeft]() {
        if (!self) return;
        self->tryFetchCatalog(installedByName, retriesLeft - 1);
    });
}

void PackageCoordinator::buildCatalogIndexes(const QVariantList& catalog)
{
    QHash<QString, QVariantList> versionsByRepoAndName;
    QHash<QString, QString>      repoByName;
    versionsByRepoAndName.reserve(catalog.size());
    repoByName.reserve(catalog.size());
    for (const QVariant& v : catalog) {
        const QVariantMap row = v.toMap();
        const QString name = row.value("name").toString();
        if (name.isEmpty()) continue;
        const QString repo = row.value("repositoryUrl").toString();
        versionsByRepoAndName.insert(catalogKey(repo, name),
                                     row.value("versions").toList());
        repoByName.insert(name, repo);
    }
    m_versionsByRepoAndName = std::move(versionsByRepoAndName);
    m_repoByName            = std::move(repoByName);
}

void PackageCoordinator::populateAppsModel(
    const QVariantList& catalog,
    const QHash<QString, QString>& installedByName)
{
    if (!m_appsModel) return;
    m_appsModel->replaceCatalog(catalog);
    m_appsModel->mergeLocalOnlyInstalled(m_installedPackagesCache);
    const QHash<QString, QString>& fullInstalled = m_installedVersionByName.isEmpty()
        ? installedByName
        : m_installedVersionByName;

    m_appsModel->beginBulkInstalledUpdate();
    for (auto it = fullInstalled.cbegin(); it != fullInstalled.cend(); ++it) {
        const QString& name = it.key();
        const QString& ver  = it.value();
        const QString hash  = m_installedHashByName.value(name);
        m_appsModel->markInstalled(name, ver, hash);
        const QString installType = m_installTypeByModule.value(name);
        if (!installType.isEmpty())
            m_appsModel->setInstallType(name, installType);
        if (m_uiPluginManager) {
            const QString iconUrl = m_uiPluginManager->pluginIconUrl(name);
            if (!iconUrl.isEmpty()) m_appsModel->setIconUrl(name, iconUrl);
        }
    }

    for (auto it = m_missingDepsByModule.cbegin();
         it != m_missingDepsByModule.cend(); ++it) {
        m_appsModel->setMissingDeps(it.key(), it.value());
    }
    m_appsModel->endBulkInstalledUpdate();

    if (m_appsLoading) {
        m_appsLoading = false;
        emit appsLoadingChanged();
    }

    // launcherApps now sources its tile color from AppsModel via colorFor().
    // Catalog arrives after the initial uiPluginsFetched/launcherAppsChanged
    // pair, so re-emit here for the sidebar to repaint with the catalog color
    // instead of the hash-fallback it picked up on first paint.
    emit launcherAppsChanged();
}

// ── Package repository management ──────────────────────────────────────────

void PackageCoordinator::refreshRepositories()
{
    LogosAPIClient* dlClient = m_logosAPI
        ? m_logosAPI->getClient("package_downloader")
        : nullptr;
    if (!dlClient || !dlClient->isConnected()) return;

    const bool wasLoading = m_repositoriesLoadingCount > 0;
    ++m_repositoriesLoadingCount;
    if (!wasLoading) emit repositoriesLoadingChanged();

    QPointer<PackageCoordinator> self(this);
    dlClient->invokeRemoteMethodAsync(
        "package_downloader", "listRepositories", QVariantList{},
        [self](QVariant result) {
            if (!self) return;
            self->m_repositories = result.toList();
            const int remaining = --self->m_repositoriesLoadingCount;
            emit self->repositoriesChanged();
            if (remaining == 0) emit self->repositoriesLoadingChanged();
        });
}

// add/remove/setEnabled share a {success, error} result shape. The
// post-success refresh happens via catalogChanged in
// subscribeToPackageDownloaderEvents, so this only forwards the outcome.
void invokeRepositoryMutation(PackageCoordinator* self,
                              LogosAPIClient* dlClient,
                              const QString& methodName,
                              const QString& operation,
                              const QString& url,
                              const QVariantList& args)
{
    QPointer<PackageCoordinator> selfPtr(self);
    dlClient->invokeRemoteMethodAsync(
        "package_downloader", methodName, args,
        [selfPtr, operation, url](QVariant result) {
            if (!selfPtr) return;
            const QVariantMap r = result.toMap();
            const bool ok = r.value("success").toBool();
            emit selfPtr->repositoryOperationCompleted(operation, url,
                ok, r.value("error").toString());
            if (ok) selfPtr->refreshRepositories();
        });
}

void PackageCoordinator::addRepository(const QString& url)
{
    LogosAPIClient* dlClient = m_logosAPI
        ? m_logosAPI->getClient("package_downloader")
        : nullptr;
    if (!dlClient || !dlClient->isConnected()) {
        emit repositoryOperationCompleted(QStringLiteral("add"), url, false,
            QStringLiteral("package_downloader not connected"));
        return;
    }
    invokeRepositoryMutation(this, dlClient, QStringLiteral("addRepository"),
                             QStringLiteral("add"), url, QVariantList{url});
}

void PackageCoordinator::removeRepository(const QString& url)
{
    LogosAPIClient* dlClient = m_logosAPI
        ? m_logosAPI->getClient("package_downloader")
        : nullptr;
    if (!dlClient || !dlClient->isConnected()) {
        emit repositoryOperationCompleted(QStringLiteral("remove"), url, false,
            QStringLiteral("package_downloader not connected"));
        return;
    }
    invokeRepositoryMutation(this, dlClient, QStringLiteral("removeRepository"),
                             QStringLiteral("remove"), url, QVariantList{url});
}

void PackageCoordinator::setRepositoryEnabled(const QString& url, bool enabled)
{
    LogosAPIClient* dlClient = m_logosAPI
        ? m_logosAPI->getClient("package_downloader")
        : nullptr;
    if (!dlClient || !dlClient->isConnected()) {
        emit repositoryOperationCompleted(QStringLiteral("setEnabled"), url, false,
            QStringLiteral("package_downloader not connected"));
        return;
    }
    invokeRepositoryMutation(this, dlClient, QStringLiteral("setRepositoryEnabled"),
                             QStringLiteral("setEnabled"), url,
                             QVariantList{url, enabled});
}

void PackageCoordinator::refreshDependencyInfo()
{
    if (!m_logosAPI) return;

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);

    // First: refresh installType for every installed package (UI + core).
    // We want the Uninstall button on both tabs to gate correctly, so the
    // map must cover everything — not just UI plugins. fetchUiPluginMetadata
    // already filled installType for its subset; we overwrite with this
    // full-scan so core-only modules pick up their installType too.
    logos.package_manager.getInstalledPackagesAsync(
        [self](QVariantList packages) {
        if (!self) return;
        if (packages.isEmpty()) {
            // Same invariant as fetchUiPluginMetadata: empty list is a failed
            // fetch, not truth — keep the previous caches.
            qWarning() << "PackageCoordinator: getInstalledPackages returned"
                          " an empty list — treating as a failed fetch,"
                          " keeping dependency caches";
            return;
        }
        self->m_installedPackagesCache = packages;
        QMap<QString, QString>   typeMap;
        QSet<QString>            nameSet;
        QHash<QString, QString>  versionByName;
        QHash<QString, QString>  hashByName;
        nameSet.reserve(packages.size());
        versionByName.reserve(packages.size());
        hashByName.reserve(packages.size());
        QMap<QString, QString> displayNameMap;
        for (const QVariant& v : packages) {
            const QVariantMap pkg = v.toMap();
            const QString name = pkg.value("name").toString();
            if (name.isEmpty()) continue;
            typeMap[name] = pkg.value("installType").toString();
            const QString dn = pkg.value("displayName").toString();
            if (!dn.isEmpty()) displayNameMap[name] = dn;
            // moduleName is the key openApp / runResolverAndOpenDialog
            // use; fall back to name when the field is absent.
            const QString lookupName = pkg.value("moduleName").toString().isEmpty()
                                           ? name
                                           : pkg.value("moduleName").toString();
            const QString version = pkg.value("version").toString();
            const QString rootHash = pkg.value("hashes").toMap().value("root").toString();
            nameSet.insert(lookupName);
            if (!version.isEmpty()) versionByName.insert(lookupName, version);
            if (!rootHash.isEmpty()) hashByName.insert(lookupName, rootHash);
        }
        self->m_installTypeByModule    = std::move(typeMap);
        self->m_displayNameByModule    = std::move(displayNameMap);
        self->m_installedNameSet       = std::move(nameSet);
        self->m_installedVersionByName = std::move(versionByName);
        self->m_installedHashByName    = std::move(hashByName);
        if (self->m_appsModel) {
            self->m_appsModel->mergeLocalOnlyInstalled(self->m_installedPackagesCache);
            self->m_appsModel->replaceInstalledSet(
                self->m_installedVersionByName, self->m_installedHashByName);
        }

        // Second pass — per-module missing/dependents queries. Dispatched
        // for every entry in the full installed-packages list (both UI and
        // core) so that QML lookups work uniformly regardless of which tab
        // is surfacing the button. `typeMap` was moved into
        // m_installTypeByModule above, so we read from the destination.
        QStringList names = self->m_installTypeByModule.keys();
        if (names.isEmpty()) {
            self->m_missingDepsByModule.clear();
            self->m_dependentsByModule.clear();
            self->m_dependenciesByModule.clear();
            // Nothing installed is a complete answer, not an unfinished one —
            // flip the gate so the UI doesn't wait forever on an empty box.
            if (!self->m_dependencyDataReady) {
                self->m_dependencyDataReady = true;
                emit self->dependencyDataReadyChanged();
            }
            emit self->uiModulesChanged();
            emit self->coreModulesChanged();
            // Sidebar reads from launcherApps (not uiModules) so it needs its
            // own kick whenever hasMissingDeps may have flipped.
            emit self->launcherAppsChanged();
            return;
        }

        LogosModules inner(self->m_logosAPI);
        auto remaining = std::make_shared<int>(names.size() * 2);
        auto missingMap = std::make_shared<QMap<QString, QStringList>>();
        auto dependenciesMap = std::make_shared<QMap<QString, QStringList>>();
        auto dependentsMap = std::make_shared<QMap<QString, QStringList>>();
        QPointer<PackageCoordinator> selfCopy(self.data());

        auto maybeFinish = [selfCopy, missingMap, dependenciesMap,
                            dependentsMap, remaining]() {
            if (!selfCopy) return;
            if (--(*remaining) > 0) return;
            selfCopy->m_missingDepsByModule = *missingMap;
            selfCopy->m_dependenciesByModule = *dependenciesMap;
            selfCopy->m_dependentsByModule = *dependentsMap;
            if (!selfCopy->m_dependencyDataReady) {
                selfCopy->m_dependencyDataReady = true;
                emit selfCopy->dependencyDataReadyChanged();
            }
            if (selfCopy->m_appsModel) {
                for (auto it = missingMap->cbegin(); it != missingMap->cend(); ++it)
                    selfCopy->m_appsModel->setMissingDeps(it.key(), it.value());
            }
            emit selfCopy->uiModulesChanged();
            emit selfCopy->coreModulesChanged();
            // Critical for the sidebar red-cross marker — without this, the
            // SidebarPanel's launcherApps binding doesn't re-evaluate and the
            // marker only appears on the next side-effect that triggers a
            // launcher refresh (e.g. clicking the plugin to load it).
            emit selfCopy->launcherAppsChanged();
        };

        for (const QString& name : names) {
            // One call, two caches. The missing-deps list is the subset with
            // status == "not_installed"; the full closure (minus those) is
            // what the uninstall plan walks. Keeping both here is why the
            // dependency cleanup needs no extra IPC.
            inner.package_manager.resolveFlatDependenciesAsync(
                name, true,
                [missingMap, dependenciesMap, name, maybeFinish](QVariantList deps) {
                    QStringList missing;
                    QStringList installed;
                    for (const QVariant& v : deps) {
                        const QVariantMap m = v.toMap();
                        const QString s = m.value("name").toString();
                        if (s.isEmpty()) continue;
                        if (m.value("status").toString() == "not_installed") missing << s;
                        else                                                 installed << s;
                    }
                    missingMap->insert(name, missing);
                    dependenciesMap->insert(name, installed);
                    maybeFinish();
                });

            inner.package_manager.resolveFlatDependentsAsync(
                name, true, [dependentsMap, name, maybeFinish](QVariantList deps) {
                    QStringList out;
                    for (const QVariant& v : deps) {
                        const QVariantMap m = v.toMap();
                        const QString s = m.value("name").toString();
                        if (!s.isEmpty()) out << s;
                    }
                    dependentsMap->insert(name, out);
                    maybeFinish();
                });
        }
    });
}

// ---------------------------------------------------------------------------
// App-Manager catalog install pipeline (ported from PMUI's
// PackageManagerBackend, adapted to PackageCoordinator's session model).
// ---------------------------------------------------------------------------

QString PackageCoordinator::buildResolverDepsJson(const QString& name,
                                                  const QString& repositoryUrl,
                                                  const QVariantMap& versionPins) const
{
    QJsonArray arr;
    QSet<QString> seenDeps;
    auto append = [&arr, &seenDeps](const QString& n, const QString& repo, const QString& ver) {
        if (n.isEmpty() || seenDeps.contains(n)) return;
        seenDeps.insert(n);
        QJsonObject obj;
        obj.insert(QStringLiteral("name"), n);
        if (!repo.isEmpty()) obj.insert(QStringLiteral("repositoryUrl"), repo);
        if (!ver.isEmpty())  obj.insert(QStringLiteral("version"), ver);
        arr.append(obj);
    };

    append(name, repositoryUrl, versionPins.value(name).toString());

    if (!repositoryUrl.isEmpty() && m_appsModel) {
        QStringList queue;
        queue << name;
        for (int head = 0; head < queue.size(); ++head) {
            const QString cur = queue[head];
            const QVariantMap row = m_appsModel->rowDataByName(cur, repositoryUrl);
            if (row.isEmpty()) continue;
            const QVariantList deps = row.value("dependencies").toList();
            for (const QVariant& d : deps) {
                const QString depName = d.toMap().value("name").toString();
                if (depName.isEmpty() || seenDeps.contains(depName)) continue;
                const QVariantMap depRow =
                    m_appsModel->rowDataByName(depName, repositoryUrl);
                if (depRow.isEmpty()) continue;  // not in this repo — leave unpinned
                append(depName, repositoryUrl, versionPins.value(depName).toString());
                queue << depName;
            }
        }
    }

    for (auto it = versionPins.cbegin(); it != versionPins.cend(); ++it) {
        const QString pinName = it.key();
        if (pinName == name || pinName.isEmpty()) continue;
        if (seenDeps.contains(pinName)) continue;
        const QString pinVersion = it.value().toString();
        if (pinVersion.isEmpty()) continue;
        append(pinName, m_repoByName.value(pinName), pinVersion);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QString PackageCoordinator::buildInstalledPackagesJson() const
{
    QJsonArray arr;
    for (const QVariant& v : m_installedPackagesCache) {
        const QVariantMap m = v.toMap();
        // package_manager rows expose both `name` and `moduleName`; the
        // resolver wants the module name. Fall back to `name` when the
        // module-name field is empty (older index shape).
        const QString name = m.value("moduleName").toString().isEmpty()
                             ? m.value("name").toString()
                             : m.value("moduleName").toString();
        const QString version = m.value("version").toString();
        if (name.isEmpty() || version.isEmpty()) continue;
        QJsonObject o;
        o.insert(QStringLiteral("name"), name);
        o.insert(QStringLiteral("version"), version);
        const QString rootHash = m.value("hashes").toMap().value("root").toString();
        if (!rootHash.isEmpty()) o.insert(QStringLiteral("rootHash"), rootHash);
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVariantMap nameAndRepo(const QString& name, const QString& repo)
{
    return {
        {QStringLiteral("name"),          name},
        {QStringLiteral("repositoryUrl"), repo},
    };
}

QVariantList PackageCoordinator::collectCatalogRequired(const QString& name,
                                                        const QString& repositoryUrl) const
{
    QVariantList out;
    QSet<QString> seen;
    out.append(nameAndRepo(name, repositoryUrl));
    seen.insert(name);

    if (repositoryUrl.isEmpty() || !m_appsModel) return out;

    QStringList queue;
    queue << name;
    for (int head = 0; head < queue.size(); ++head) {
        const QVariantMap row = m_appsModel->rowDataByName(queue[head], repositoryUrl);
        if (row.isEmpty()) continue;
        const QVariantList deps = row.value("dependencies").toList();
        for (const QVariant& d : deps) {
            const QString depName = d.toMap().value("name").toString();
            if (depName.isEmpty() || seen.contains(depName)) continue;
            if (m_appsModel->rowDataByName(depName, repositoryUrl).isEmpty()) continue;
            seen.insert(depName);
            out.append(nameAndRepo(depName, repositoryUrl));
            queue << depName;
        }
    }
    return out;
}

QString PackageCoordinator::depAction(const QString& installedVersion,
                                      const QString& resolvedVersion,
                                      const QString& installedHash,
                                      const QString& resolvedHash)
{
    if (installedVersion.isEmpty()) return QStringLiteral("install");
    if (installedVersion == resolvedVersion) {
        const bool hashKnown = !installedHash.isEmpty() && !resolvedHash.isEmpty();
        if (hashKnown && installedHash != resolvedHash)
            return QStringLiteral("reinstall");
        return QStringLiteral("installed");
    }
    return QStringLiteral("upgrade");
}

bool PackageCoordinator::installPluginSucceeded(const QVariantMap& installResult)
{
    return installResult.value(QStringLiteral("error")).toString().isEmpty();
}

QVariantMap PackageCoordinator::changeFromResolverEntry(const QVariantMap& entry,
                                                        const QString& installedVersion,
                                                        const QString& installedHash)
{
    if (entry.contains("error")) {
        return {
            {QStringLiteral("name"),   entry.value("name")},
            {QStringLiteral("action"), QStringLiteral("error")},
            {QStringLiteral("error"),  entry.value("error")},
        };
    }
    const QString to  = entry.value("version").toString();
    const QString toH = entry.value("rootHash").toString();
    return {
        {QStringLiteral("name"),          entry.value("name").toString()},
        {QStringLiteral("toVersion"),     to},
        {QStringLiteral("fromVersion"),   installedVersion},
        {QStringLiteral("repositoryUrl"), entry.value("repositoryUrl").toString()},
        {QStringLiteral("description"),   entry.value("description").toString()},
        {QStringLiteral("action"),        depAction(installedVersion, to, installedHash, toH)},
        {QStringLiteral("isTopLevel"),    entry.value("topLevel").toBool()},
    };
}

QVariantList PackageCoordinator::computeDepChanges(
    const QVariantList& resolved,
    const QHash<QString, QString>& installedByName) const
{
    QVariantList out;
    for (const QVariant& v : resolved) {
        const QVariantMap m = v.toMap();
        const QString name = m.value("name").toString();
        QVariantMap c = changeFromResolverEntry(
            m, installedByName.value(name), m_installedHashByName.value(name));
        if (c.value("action").toString() == QStringLiteral("error")) {
            out.append(c);
            continue;
        }
        const QString repoUrl = c.value("repositoryUrl").toString();
        c.insert(QStringLiteral("versions"),
                 m_versionsByRepoAndName.value(catalogKey(repoUrl, name)));
        if (c.value("isTopLevel").toBool()) out.prepend(c);
        else                                out.append(c);
    }
    return out;
}

void PackageCoordinator::setOpStage(const QString& name, InstallStage::Value stage)
{
    if (!m_installRegistry->has(name)) return;
    if (m_installRegistry->stage(name) == static_cast<int>(stage)) return;
    m_installRegistry->setStage(name, stage);
    emit catalogInstallStageChanged(name, stage);
}

void PackageCoordinator::openApp(const QString& name,
                                 const QString& repositoryUrl,
                                 const QVariantMap& versionPins,
                                 bool allowFastLaunch)
{
    if (!m_logosAPI || name.isEmpty()) return;

    // Fast-launch only for the tile whose repo's rootHash matches what's
    // on disk. installStatus is already missing-deps-aware: AppsModel's
    // recomputeInstallStatus demotes a row with non-empty missingDeps to
    // NotInstalled, so tileStatus == Installed implies healthy deps.
    int tileStatus = InstallStatus::NotInstalled;
    if (m_appsModel) {
        const QVariantMap row =
            m_appsModel->rowDataByName(name, repositoryUrl);
        tileStatus = row.value("installStatus").toInt();
    }
    if (allowFastLaunch && tileStatus == InstallStatus::Installed) {
        qDebug() << "openApp fast-path: installed (v="
                 << m_installedVersionByName.value(name)
                 << "), emitting launchAppRequested";
        emit launchAppRequested(name);
        return;
    }

    runResolverAndOpenDialog(name, repositoryUrl, versionPins);
}

void PackageCoordinator::notifyAddApplicationDialogClosed()
{
    if (m_activeAddDialogName.isEmpty()) return;
    ++m_dialogResolveEpoch[m_activeAddDialogName];
    m_activeAddDialogName.clear();
}

void PackageCoordinator::runResolverAndOpenDialog(const QString& name,
                                                  const QString& repositoryUrl,
                                                  const QVariantMap& versionPins)
{
    QVariantMap catalogRow =
        m_appsModel ? m_appsModel->rowDataByName(name, repositoryUrl) : QVariantMap{};

    const QString targetVersion = versionPins.value(name).toString();

    const int epoch = ++m_dialogResolveEpoch[name];
    m_activeAddDialogName = name;

    const QString depsJson = buildResolverDepsJson(name, repositoryUrl, versionPins);

    qDebug() << "PackageCoordinator::runResolverAndOpenDialog" << name
             << "repo=" << repositoryUrl << "targetVersion=" << targetVersion
             << "pins=" << versionPins.size() << "epoch=" << epoch;

    QVariantList initialChanges;
    if (m_installRegistry->isInFlight(name))
        initialChanges = m_lastResolvedChangesByName.value(name);
    // Sync stack frame only — QML may open the modal from this signal.
    emitDialogMetadata(name, repositoryUrl, targetVersion, catalogRow, initialChanges,
                       /*requestOpen=*/true);

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_downloader.resolveDependenciesAsync(depsJson, QString(),
        [self, name, repositoryUrl, targetVersion, catalogRow, epoch]
        (QVariantList resolved) {
            if (!self) return;
            if (self->m_dialogResolveEpoch.value(name) != epoch) {
                qDebug() << "runResolverAndOpenDialog: dropping superseded epoch"
                         << epoch << "for" << name;
                return;
            }
            const QVariantList changes =
                self->computeDepChanges(resolved, self->m_installedVersionByName);
            if (!resolved.isEmpty())
                self->m_lastResolvedRawByName.insert(name, resolved);
            if (!changes.isEmpty())
                self->m_lastResolvedChangesByName.insert(name, changes);
            // Async refresh only — never reopens the modal.
            self->emitDialogMetadata(name, repositoryUrl, targetVersion, catalogRow, changes,
                                     /*requestOpen=*/false);
        });
}

void PackageCoordinator::emitDialogMetadata(const QString& name,
                                            const QString& repositoryUrl,
                                            const QString& targetVersion,
                                            const QVariantMap& catalogRow,
                                            const QVariantList& changes,
                                            bool requestOpen)
{
    if (name != m_activeAddDialogName)
        return;

    QVariantMap metadata;
    metadata["name"]          = name;
    metadata["repositoryUrl"] = repositoryUrl;
    metadata["selectedVersion"] = targetVersion;
    metadata["displayName"]   = catalogRow.value("displayName").toString().isEmpty()
                                    ? name
                                    : catalogRow.value("displayName");
    metadata["description"]   = catalogRow.value("description");
    metadata["icon"]          = catalogRow.value("iconUrl");
    metadata["color"]         = catalogRow.value("color");
    metadata["category"]      = catalogRow.value("category");
    metadata["versions"]      = catalogRow.value("versions").toList();
    const QString installedVersion = m_installedVersionByName.value(name);
    // Per-tile installStatus from the CLICKED row. Already missing-deps-aware
    // (AppsModel::recomputeInstallStatus returns NotInstalled when missingDeps
    // is non-empty), so the dialog reads Install instead of Launch for a
    // partial install.
    int tileStatus = InstallStatus::NotInstalled;
    if (m_appsModel) {
        const QVariantMap clickedRow = m_appsModel->rowDataByName(name, repositoryUrl);
        tileStatus = clickedRow.value("installStatus").toInt();
    }
    metadata["installStatus"]    = tileStatus;
    metadata["isInstalled"]      = tileStatus == InstallStatus::Installed;
    metadata["installedVersion"] = installedVersion;
    // Needed by the dialog's Uninstall affordance: "embedded" packages ship
    // inside the bundle and package_manager refuses to remove them, so the
    // button has to hide rather than offer a call that always fails.
    metadata["installType"]      = m_installTypeByModule.value(name);
    const QVariantList versionsList = catalogRow.value("versions").toList();
    metadata["latestVersion"] = versionsList.isEmpty()
        ? QString()
        : versionsList.first().toMap().value("manifest").toMap().value("version").toString();

    metadata["installStage"] = m_installRegistry->stage(name);

    // {name, repo} entries so the filter pins each row to the resolver's
    // chosen repo and multi-repo names don't duplicate. Always at least the
    // top-level entry so the dialog has something to render before the
    // async resolver callback arrives.
    QVariantList requiredEntries;
    QSet<QString> seen;
    requiredEntries.reserve(changes.size() + 1);
    requiredEntries.append(nameAndRepo(name, repositoryUrl));
    seen.insert(name);
    if (m_appsModel) {
        QList<AppsModel::ResolverRow> overlay;
        overlay.reserve(changes.size());
        for (const QVariant& v : changes) {
            const QVariantMap c = v.toMap();
            AppsModel::ResolverRow rr;
            rr.name          = c.value("name").toString();
            rr.repositoryUrl = c.value("repositoryUrl").toString();
            rr.action        = c.value("action").toString();
            rr.toVersion     = c.value("toVersion").toString();
            rr.isTopLevel    = c.value("isTopLevel").toBool();
            rr.resolverError = c.value("error").toString();
            overlay.append(rr);
            if (!rr.name.isEmpty() && !seen.contains(rr.name)) {
                seen.insert(rr.name);
                requiredEntries.append(
                    nameAndRepo(rr.name, c.value("repositoryUrl").toString()));
            }
        }
        m_appsModel->setResolverOverlay(overlay);
    }

    // Union in the catalog-derived dependency set.
    for (const QVariant& v : collectCatalogRequired(name, repositoryUrl)) {
        const QString depName = v.toMap().value("name").toString();
        if (depName.isEmpty() || seen.contains(depName)) continue;
        seen.insert(depName);
        requiredEntries.append(v);
    }

    if (m_requiredPackagesModel)
        m_requiredPackagesModel->setRequiredPackages(requiredEntries);

    if (requestOpen)
        emit requestOpenAddApplicationDialog(metadata);
    else
        emit addApplicationDataUpdated(metadata);
}

void PackageCoordinator::refreshOverlayAfterInstall(const QString& topLevelName)
{
    if (!m_appsModel || topLevelName.isEmpty()) return;

    const QVariantList raw = m_lastResolvedRawByName.value(topLevelName);
    if (raw.isEmpty()) return;

    const QVariantList changes =
        computeDepChanges(raw, m_installedVersionByName);
    if (!changes.isEmpty())
        m_lastResolvedChangesByName.insert(topLevelName, changes);

    // Only push UI updates while this app's dialog is still the active session.
    if (topLevelName != m_activeAddDialogName) return;

    const QString repositoryUrl = m_repoByName.value(topLevelName);
    const QVariantMap catalogRow =
        m_appsModel->rowDataByName(topLevelName, repositoryUrl);
    emitDialogMetadata(topLevelName, repositoryUrl, QString(), catalogRow, changes,
                       /*requestOpen=*/false);
}

void PackageCoordinator::confirmCatalogInstall(const QString& name,
                                                const QString& repositoryUrl,
                                                const QVariantMap& versionPins)
{
    if (!m_logosAPI || name.isEmpty()) return;

    if (m_installRegistry->has(name)) {
        qDebug() << "confirmCatalogInstall: session for" << name
                 << "already in progress, ignoring";
        return;
    }

    m_installRegistry->begin(name, /*targetVersion=*/{}, /*targetHash=*/{},
                        /*startedByTopLevel=*/name);
    emit catalogInstallStageChanged(name, InstallStage::Downloading);

    const QString depsJson = buildResolverDepsJson(name, repositoryUrl, versionPins);

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    // Default IPC deadline (20s) is too tight when the catalog blob is many
    // MB or the user is on a slow connection
    constexpr int kDownloadIpcDeadlineMs = 5 * 60 * 1000;
    logos.package_downloader.downloadResolvedDependenciesAsync(depsJson, QString(),
        [self, name](QVariantList results) {
            if (!self) return;
            if (!results.isEmpty())
                self->m_lastResolvedRawByName.insert(name, results);

            QVariantList toInstall;
            for (const QVariant& v : results) {
                const QVariantMap m = v.toMap();
                const QString rowName = m.value("name").toString();
                if (!m.value("error").toString().isEmpty()) {
                    toInstall.append(v);
                    continue;
                }
                const QString resolvedVersion  = m.value("version").toString();
                const QString resolvedHash     = m.value("rootHash").toString();
                const QString installedVersion = self->m_installedVersionByName.value(rowName);
                const QString installedHash    = self->m_installedHashByName.value(rowName);
                const bool versionMatches =
                    !installedVersion.isEmpty()
                    && (resolvedVersion.isEmpty()
                        || resolvedVersion == installedVersion);
                const bool hashMatches =
                    resolvedHash.isEmpty()
                    || installedHash.isEmpty()
                    || resolvedHash == installedHash;
                if (versionMatches && hashMatches) {
                    self->m_installRegistry->beginOrTrack(rowName, resolvedVersion,
                                                    resolvedHash, name);
                    self->m_installRegistry->setStage(rowName, InstallStage::Installed);
                    continue;
                }
                toInstall.append(v);
            }

            if (toInstall.isEmpty()) {
                // Nothing left to do after the skip-already-installed
                // filter; treat as a successful no-op rather than Failed.
                self->setOpStage(name, InstallStage::Installed);
                emit self->catalogInstallFinished(name);
                self->refreshOverlayAfterInstall(name);
                QTimer::singleShot(1500, self.data(), [self, name]() {
                    if (!self) return;
                    self->m_installRegistry->clearByTopLevel(name);
                });
                return;
            }

            for (const QVariant& v : toInstall) {
                const QVariantMap m = v.toMap();
                const QString rowName = m.value("name").toString();
                if (rowName.isEmpty()) continue;
                self->m_installRegistry->beginOrTrack(rowName,
                    m.value("version").toString(),
                    m.value("rootHash").toString(),
                    name);
                self->m_installRegistry->setStage(rowName, InstallStage::Queued);
            }

            self->setOpStage(name, InstallStage::Installing);
            self->installResultsSequential(toInstall, name, 0);
        },
        Timeout(kDownloadIpcDeadlineMs));
}

void PackageCoordinator::installOnePackage(const QVariantMap& dl,
    std::function<void(bool, const QString&)> onDone)
{
    const QString packageName  = dl.value("name").toString();
    const QString filePath     = dl.value("path").toString();
    const QString downloadError = dl.value("error").toString();

    if (filePath.isEmpty()) {
        if (onDone) onDone(false, downloadError.isEmpty()
                                       ? QStringLiteral("Download failed")
                                       : downloadError);
        return;
    }

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.installPluginAsync(filePath, false,
        [self, packageName, onDone](QVariantMap installResult) {
            if (!self) return;
            const bool success = installPluginSucceeded(installResult);
            const QString err  = installResult.value("error").toString();
            if (onDone) onDone(success, success ? QString() : err);
        });
}

void PackageCoordinator::installResultsSequential(const QVariantList& results,
                                                  const QString& topLevelName,
                                                  int index,
                                                  QStringList failures)
{
    if (index >= results.size()) return;
    const QVariantMap dl = results[index].toMap();
    const QString rowName = dl.value("name").toString();
    qDebug() << "installResultsSequential index=" << index
             << "of" << results.size()
             << "rowName=" << rowName
             << "topLevel=" << topLevelName;
    if (!rowName.isEmpty()) {
        m_installRegistry->beginOrTrack(rowName, dl.value("version").toString(),
                                   dl.value("rootHash").toString(), topLevelName);
        m_installRegistry->setStage(rowName, InstallStage::Installing);
    }

    QPointer<PackageCoordinator> self(this);
    installOnePackage(dl,
        [self, results, topLevelName, rowName, index, failures, dl]
        (bool success, const QString& err) mutable {
            qDebug() << "installOnePackage callback rowName=" << rowName
                     << "success=" << success << "err=" << err;
            if (!self) return;

            if (!rowName.isEmpty()) {
                if (success) {
                    const QString ver = dl.value("version").toString();
                    const QString hash = dl.value("rootHash").toString();
                    if (!ver.isEmpty())
                        self->m_installedVersionByName.insert(rowName, ver);
                    if (!hash.isEmpty())
                        self->m_installedHashByName.insert(rowName, hash);
                    if (self->m_appsModel)
                        self->m_appsModel->markInstalled(rowName, ver, hash);
                    self->m_installRegistry->finish(rowName);
                } else {
                    self->m_installRegistry->fail(rowName, err);
                }
            }
            if (!success) {
                failures.append(rowName.isEmpty()
                                    ? err
                                    : (rowName + ": " + err));
            }

            // Stop on the first failure rather than growing a half-installed
            // set; report it now. (Rollback of installed packages is follow-up.)
            const bool isLast = (index + 1) >= results.size();
            if (!isLast && success) {
                self->installResultsSequential(
                    results, topLevelName, index + 1, failures);
                return;
            }

            if (!failures.isEmpty()) {
                qDebug() << "  install loop complete with failures for"
                         << topLevelName << ":" << failures.size();
                self->setOpStage(topLevelName, InstallStage::Failed);
                emit self->catalogInstallFailed(
                    topLevelName, failures.join(QStringLiteral("; ")));
                QTimer::singleShot(2500, self.data(), [self, topLevelName]() {
                    if (!self) return;
                    self->m_installRegistry->clearByTopLevel(topLevelName);
                });
                return;
            }

            self->setOpStage(topLevelName, InstallStage::Installed);
            emit self->catalogInstallFinished(topLevelName);
            self->refreshOverlayAfterInstall(topLevelName);
            QTimer::singleShot(1500, self.data(), [self, topLevelName]() {
                if (!self) return;
                self->m_installRegistry->clearByTopLevel(topLevelName);
            });
        });
}
