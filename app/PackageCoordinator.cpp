#include "PackageCoordinator.h"
#include "ResolverRequest.h"
#include "InstallRegistry.h"
#include "AppsModel.h"
#include "CoreModuleManager.h"
#include "UIPluginManager.h"
#include "LogosBasecampPaths.h"
#include "utils/DependencyBlocker.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <memory>

#include "LogosIntent.h"

#include <logos/semver.hpp>

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
    subscribeToPackageInstallationEvents();
    subscribeToPackageDownloaderEvents();

    // A module absent at startup may be installed and loaded later in the
    // session. Without this the guards above would turn a 6-minute stall into a
    // silently non-functional Modules view, which is a worse bug. Both
    // subscribe functions are idempotent, so re-running them is free once armed.
    if (m_coreModuleManager) {
        connect(m_coreModuleManager, &CoreModuleManager::coreModulesChanged,
                this, [this]() {
                    subscribeToPackageInstallationEvents();
                    subscribeToPackageDownloaderEvents();
                });
    }

    // NB: initial metadata fetch is deferred until MainUIBackend calls
    // refresh() — the uiPluginsFetched signal would otherwise fire before
    // UIPluginManager's setPackageCoordinator runs and the slot connection
    // lands, causing the first-paint UI-plugin list to be empty until the
    // next file-install event triggers a re-scan.
}

PackageCoordinator::~PackageCoordinator() = default;

namespace {

// Is a module actually loaded in THIS process?
//
// The obvious guard -- `client->isConnected()` -- was dead code: QtRO's
// connectToNode() only validates the URL scheme and never contacts a peer, so
// it reported "connected" for modules that were never loaded. Every call past
// it then blocked 20 s in waitForSource, twice over (the token handshake tries
// capability_module first). Measured cost with package_manager absent: ~417 s
// of blocked GUI thread on macOS and 361 s on Linux before Basecamp's window
// appeared, because all of this runs inside the Window constructor.
//
// logos_core_get_loaded_modules answers the same question in-process, with no
// IPC and no timeout. The same guard already ships in
// logos-logoscore-cli/src/daemon/daemon.cpp, which skips its identical
// setEmbeddedModulesDirectory block and reports that package commands are
// unavailable for the session.
bool moduleIsLoaded(CoreModuleManager* core, const QString& name)
{
    if (!core) {
        // No oracle available: keep the previous behaviour rather than silently
        // disabling package management.
        return true;
    }
    return core->loadedModules().contains(name);
}

} // namespace

void PackageCoordinator::subscribeToPackageInstallationEvents()
{
    if (!m_logosAPI) {
        return;
    }

    if (m_packageManagerSubscribed) {
        return;
    }
    if (!moduleIsLoaded(m_coreModuleManager, "package_manager")) {
        if (!m_warnedPackageManagerMissing) {
            m_warnedPackageManagerMissing = true;
            qWarning() << "PackageCoordinator: package_manager is not loaded -- skipping its "
                          "directory setup and event subscriptions. Package management is "
                          "unavailable until it loads; this will be retried automatically.";
        }
        return;
    }
    m_packageManagerSubscribed = true;

    LogosModules logos(m_logosAPI);

    // Configure the package_manager module's directories so it knows where
    // to install.
    logos.package_manager.setEmbeddedModulesDirectory(LogosBasecampPaths::embeddedModulesDirectory());
    logos.package_manager.setUserModulesDirectory(LogosBasecampPaths::modulesDirectory());
    logos.package_manager.setEmbeddedUiPluginsDirectory(LogosBasecampPaths::embeddedPluginsDirectory());
    logos.package_manager.setUserUiPluginsDirectory(LogosBasecampPaths::pluginsDirectory());

    logos.package_manager.on("corePluginFileInstalled", [this](const QVariantList& data) {
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

    logos.package_manager.on("uiPluginFileInstalled", [this](const QVariantList& data) {
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
    logos.package_manager.on("corePluginUninstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "Core module uninstalled:" << data[0].toString();
        QTimer::singleShot(100, this, [this]() {
            if (m_coreModuleManager) m_coreModuleManager->refresh();
            fetchUiPluginMetadata();
        });
    });

    logos.package_manager.on("uiPluginUninstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "UI plugin uninstalled:" << data[0].toString();
        QTimer::singleShot(100, this, [this]() {
            fetchUiPluginMetadata();
        });
    });

}

void PackageCoordinator::subscribeToPackageDownloaderEvents()
{
    if (!m_logosAPI) return;

    if (m_packageDownloaderSubscribed) {
        return;
    }
    if (!moduleIsLoaded(m_coreModuleManager, "package_downloader")) {
        if (!m_warnedPackageDownloaderMissing) {
            m_warnedPackageDownloaderMissing = true;
            qWarning() << "PackageCoordinator: package_downloader is not loaded -- skipping its "
                          "event subscriptions; this will be retried automatically.";
        }
        return;
    }
    m_packageDownloaderSubscribed = true;

    LogosModules logos(m_logosAPI);
    logos.package_downloader.on("catalogChanged", [this](const QVariantList&) {
        refreshRepositories();
        refresh();
    });
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

QVariantList PackageCoordinator::blockingDepsOf(const QString& name) const
{
    return m_blockingDepsByModule.value(name);
}

QStringList PackageCoordinator::dependentsOf(const QString& name) const
{
    return m_dependentsByModule.value(name);
}

QString PackageCoordinator::installedRootHash(const QString& name) const
{
    return m_installedHashByName.value(name);
}

QString PackageCoordinator::displayNameFor(const QString& name) const
{
    const QString dn = m_displayNameByModule.value(name);
    if (!dn.isEmpty()) return dn;
    return name;
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

    beginLocalUninstall({moduleName}, QStringLiteral("packages"));
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
    // actually picked apart from what the batch dragged in.
    m_lastRequestedTargets = {name};
    beginLocalUninstall(plan.batch, QStringLiteral("app"));
}

void PackageCoordinator::cancelPendingUninstallApp(const QString& name)
{
    if (m_pendingUninstallAppName != name) return;
    m_pendingUninstallAppName.clear();
    QObject::disconnect(m_pendingUninstallAppConn);
}

void PackageCoordinator::uninstallCoreModule(const QString& moduleName)
{
    // Same flow as uninstallUiModule — removal is type-agnostic.
    qDebug() << "uninstallCoreModule:" << moduleName;
    beginLocalUninstall({moduleName}, QStringLiteral("packages"));
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
                                                 bool multi,
                                                 const QString& requesterName) const
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
        // Empty when the shell itself asked (Settings → Modules / Apps).
        {QStringLiteral("requester"),  requesterName},
        {QStringLiteral("requesterBundled"), requesterIsBundled(requesterName)},
        {QStringLiteral("requesterDisplayName"),
             requesterName.isEmpty() ? QString() : displayNameFor(requesterName)},
    };
}

// ---------------------------------------------------------------------------
// Cascade confirmation — triggered from QML once the user OKs the dialog.
// ---------------------------------------------------------------------------

void PackageCoordinator::cascadeUnloadForPackage(const QString& moduleName)
{
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
}

void PackageCoordinator::confirmUninstallCascade(const QString& moduleName)
{
    if (m_pendingAction.op != PendingOp::UninstallCascade
        || m_pendingAction.name != moduleName) {
        qWarning() << "confirmUninstallCascade for" << moduleName
                   << "but pending action is" << m_pendingAction.name;
        return;
    }

    const QString requestId = m_pendingAction.intentRequestId;
    const bool    isUpgrade = m_pendingAction.isUpgrade;
    m_pendingAction = {};

    QPointer<PackageCoordinator> selfDefer(this);
    QMetaObject::invokeMethod(this, [this, selfDefer, moduleName, requestId, isUpgrade]() {
        if (!selfDefer) return;

        cascadeUnloadForPackage(moduleName);

        // Answering is what tells the requester it may now remove files. If
        // nobody heard — no intent, or a dispatch that already ended, including
        // the case where the cascade just tore down the requester itself — the
        // removal falls to us, or an uninstall would leave the package unloaded
        // but still on disk with nothing to finish the job.
        //
        // NOT for an upgrade. There the requester was going to remove the old
        // version and install the new one; doing only the removal ourselves
        // would delete the package outright. Leave it installed (unloaded) —
        // the requester has already told the user the action failed.
        if (!finishIntent(requestId, true) && !isUpgrade)
            performLocalRemoval({moduleName});

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
    if (m_pendingAction.op != PendingOp::UninstallCascade
        || m_pendingAction.name != moduleName) {
        // MainUIBackend fans out cancelPendingAction to both managers so one
        // of them is always a no-op — don't even warn here.
        return;
    }
    qDebug() << "Cancelling pending package action for" << moduleName;
    const QString requestId = m_pendingAction.intentRequestId;
    m_pendingAction = {};
    finishIntent(requestId, false, logos::intent::errCancelled());
}

// ---------------------------------------------------------------------------
// Package-confirmation intents
//
// package_manager_ui asks; we draw the dialog, run the cascade-unload, and only
// then answer. The response IS the permission — PMU deletes nothing until it
// arrives, so the unload has always finished first.
// ---------------------------------------------------------------------------

bool PackageCoordinator::finishIntent(const QString& requestId, bool ok,
                                      const QString& error)
{
    if (requestId.isEmpty()) return false;
    if (!m_intentResponder) return false;
    return m_intentResponder(requestId, ok, error);
}

void PackageCoordinator::beginLocalUninstall(const QStringList& names,
                                             const QString& kind)
{
    if (names.isEmpty()) return;
    if (m_pendingAction.op != PendingOp::None) {
        qWarning() << "beginLocalUninstall: a confirmation is already open — ignoring";
        return;
    }
    const bool multi = names.size() > 1;

    // No intentRequestId: we asked ourselves, so the removal is ours to do.
    m_pendingAction = multi
        ? PendingAction{PendingOp::MultiUninstallCascade, {}, names, {}}
        : PendingAction{PendingOp::UninstallCascade, names.first(), {}, {}};

    // Empty requester — the dialog says nothing about who asked, because the
    // user is the one who clicked.
    emit uninstallPlanRequested(
        buildPlanPayload(names, dependentsOfBatch(names), kind, multi));
    m_lastRequestedTargets.clear();
}

void PackageCoordinator::performLocalRemoval(const QStringList& names)
{
    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    for (const QString& name : names) {
        logos.package_manager.uninstallPackageAsync(name, [name](QVariantMap r) {
            if (!r.value("success", false).toBool())
                qWarning() << "uninstallPackage failed for" << name << ":"
                           << r.value("error").toString();
        });
    }
}

void PackageCoordinator::resolveDepChangesThen(const QString& name,
                                               const QString& repositoryUrl,
                                               const QString& version,
                                               std::function<void(bool, const QVariantList&)> then)
{
    // No repo to resolve against (every local .lgx), or no downloader. Answer
    // immediately rather than leaving the dialog unopened — but as UNRESOLVED,
    // so it says the dependencies are unknown instead of claiming there are
    // none.
    if (repositoryUrl.isEmpty() || !m_logosAPI) {
        then(false, {});
        return;
    }

    QVariantMap pins;
    if (!version.isEmpty()) pins.insert(name, version);
    const QString depsJson =
        logos::gateResolverRequest(name, repositoryUrl, version);

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.getInstalledPackagesAsync(
        [self, depsJson, then](QVariantList installed) {
        if (!self) return;
        LogosModules logos(self->m_logosAPI);
        logos.package_downloader.resolveDependenciesAsync(
            depsJson, logos::installedPackagesJson(installed),
        [self, then](QVariantList resolved) {
            if (!self) return;   // we are gone; the pending intent dies with us
            QVariantList changes;
            for (const QVariant& v : resolved) {
                const QVariantMap entry = v.toMap();
                const QString entryName = entry.value("name").toString();
                if (entryName.isEmpty()) continue;
                // The resolver echoes the requested package back as topLevel.
                // It is the subject of the dialog, not one of its dep changes.
                if (entry.value("topLevel").toBool()) continue;
                changes.append(changeFromResolverEntry(
                    entry,
                    self->m_installedVersionByName.value(entryName),
                    self->m_installedHashByName.value(entryName)));
            }
            then(true, changes);
        });
    });
}

bool PackageCoordinator::requesterIsBundled(const QString& requesterName) const
{
    // Embedded means it came out of our own bundle — provenance the shell can
    // actually vouch for, and stronger than any signature we could check today.
    // Anything else, INCLUDING a user-installed package that has taken the
    // bundled component's name, is not vouched for. Unknown counts as not
    // bundled: the cache is empty until the first scan completes, and the safe
    // default there is to say so rather than imply trust we have not checked.
    return !requesterName.isEmpty()
        && installType(requesterName) == QLatin1String("embedded");
}

QStringList PackageCoordinator::dependentsOfBatch(const QStringList& names) const
{
    // From OUR cache, never the payload: a caller-supplied list would let a
    // removal be dressed up as harmless, which is the one thing this dialog
    // exists to show honestly. Members of the batch are not their own casualties.
    QStringList out;
    for (const QString& n : names)
        for (const QString& d : dependentsOf(n))
            if (!out.contains(d) && !names.contains(d))
                out.append(d);
    return out;
}

bool PackageCoordinator::beginPackageConfirmation(const QString& dispatchId,
                                                  const QString& intent,
                                                  const QVariantMap& params,
                                                  const QString& requesterName)
{
    // Shell-provided intents skip the chooser and dispatch straight through, so
    // two really can arrive back-to-back (a bulk "Run Actions" fires one per
    // row). Only one dialog can own the screen, so a second is refused — and
    // refusing has to be an ANSWER, not a dropped dispatch, or the requester
    // waits out the broker's full response deadline for a reply we already
    // decided not to give.
    const bool busy = !m_pendingInstallRequestId.isEmpty()
                   || m_pendingAction.op != PendingOp::None;
    if (busy) {
        qWarning() << "PackageCoordinator: confirmation in flight, refusing" << intent;
        finishIntent(dispatchId, false, logos::intent::errFailed());
        return true;   // answered — do NOT let the broker time it out too
    }

    const QString name    = params.value(QStringLiteral("name")).toString();
    const QString version = params.value(QStringLiteral("version")).toString();

    if (intent == QLatin1String("logos.packages.confirm_install")) {
        if (name.isEmpty()) {
            finishIntent(dispatchId, false, logos::intent::errBadRequest());
            return true;
        }
        // A fresh install unloads nothing, so there is no cascade and no
        // PendingAction — the dialog's answer is the whole flow.
        m_pendingInstallRequestId = dispatchId;
        m_pendingInstallName      = name;
        resolveDepChangesThen(name, params.value(QStringLiteral("repositoryUrl")).toString(),
            version, [this, name, version, requesterName](bool resolved,
                                                          const QVariantList& changes) {
                emit installGateConfirmationRequested(name, version, changes, requesterName,
                                                     requesterIsBundled(requesterName),
                                                     resolved);
            });
        return true;
    }

    if (intent == QLatin1String("logos.packages.confirm_upgrade")) {
        if (name.isEmpty()) {
            finishIntent(dispatchId, false, logos::intent::errBadRequest());
            return true;
        }
        const int mode = params.value(QStringLiteral("mode")).toInt();
        const QStringList installedDeps = dependentsOfBatch({name});
        const QStringList loadedDeps = m_uiPluginManager
            ? m_uiPluginManager->intersectWithLoaded(installedDeps)
            : QStringList{};
        m_pendingAction = {PendingOp::UninstallCascade, name, {}, dispatchId,
                           /*isUpgrade=*/true};
        resolveDepChangesThen(name, params.value(QStringLiteral("repositoryUrl")).toString(),
            version, [this, name, version, mode, installedDeps, loadedDeps, requesterName]
            (bool /*resolved*/, const QVariantList& changes) {
                emit upgradeCascadeConfirmationRequested(name, version, mode, installedDeps,
                                                        loadedDeps, changes, requesterName,
                                                        requesterIsBundled(requesterName));
            });
        return true;
    }

    if (intent == QLatin1String("logos.packages.confirm_uninstall")) {
        QStringList names;
        for (const QVariant& v : params.value(QStringLiteral("names")).toList()) {
            const QString n = v.toString();
            if (!n.isEmpty() && !names.contains(n)) names.append(n);
        }
        if (names.isEmpty()) {
            finishIntent(dispatchId, false, logos::intent::errBadRequest());
            return true;
        }

        const bool multi = names.size() > 1;
        const QVariantMap plan =
            buildPlanPayload(names, dependentsOfBatch(names),
                             QStringLiteral("packages"), multi, requesterName);
        if (plan.value(QStringLiteral("removable")).toList().isEmpty()) {
            qWarning() << "PackageCoordinator: nothing removable in" << names
                       << "— refusing" << intent;
            finishIntent(dispatchId, false, logos::intent::errBadRequest());
            return true;
        }

        m_pendingAction = multi
            ? PendingAction{PendingOp::MultiUninstallCascade, {}, names, dispatchId}
            : PendingAction{PendingOp::UninstallCascade, names.first(), {}, dispatchId};

        emit uninstallPlanRequested(plan);
        return true;
    }

    qWarning() << "PackageCoordinator: unhandled package intent" << intent;
    finishIntent(dispatchId, false, logos::intent::errFailed());
    return true;
}

// A fresh install unloads nothing, so both of these are just the answer — but
// only for the package the pending dialog actually names.
void PackageCoordinator::confirmInstallGate(const QString& name)
{
    if (m_pendingInstallName != name) {
        qWarning() << "confirmInstallGate for" << name
                   << "but pending install is" << m_pendingInstallName;
        return;
    }
    const QString requestId = m_pendingInstallRequestId;
    m_pendingInstallRequestId.clear();
    m_pendingInstallName.clear();
    finishIntent(requestId, true);
}

void PackageCoordinator::cancelInstallGate(const QString& name)
{
    if (m_pendingInstallName != name) return;
    const QString requestId = m_pendingInstallRequestId;
    m_pendingInstallRequestId.clear();
    m_pendingInstallName.clear();
    finishIntent(requestId, false, logos::intent::errCancelled());
}

void PackageCoordinator::confirmUninstallMultiCascade(const QStringList& moduleNames)
{
    if (m_pendingAction.op != PendingOp::MultiUninstallCascade
        || m_pendingAction.names != moduleNames) {
        qWarning() << "confirmUninstallMultiCascade: no matching pending action — ignoring";
        return;
    }
    const QString requestId = m_pendingAction.intentRequestId;
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

    // Everything in the batch is unloaded — tell the requester it may remove
    // them; if nobody heard, the removal falls to us.
    if (!finishIntent(requestId, true))
        performLocalRemoval(moduleNames);

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
    const QString requestId = m_pendingAction.intentRequestId;
    m_pendingAction = {};
    finishIntent(requestId, false, logos::intent::errCancelled());
}

// ---------------------------------------------------------------------------
// Metadata refresh chain
// ---------------------------------------------------------------------------

void PackageCoordinator::fetchUiPluginMetadata()
{
    // The !m_logosAPI branch below already does the right thing when package
    // metadata is unavailable -- clear the loading state and tell the UI -- so
    // an absent package_manager takes the same path rather than blocking 20 s
    // acquiring a token for a module that is not there. Without this the app
    // would show its window and then sit in a loading state for the timeout.
    if (!moduleIsLoaded(m_coreModuleManager, "package_manager")) {
        if (m_appsLoading) {
            m_appsLoading = false;
            emit appsLoadingChanged();
        }
        emit uiModulesChanged();
        return;
    }

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
            if (!iconUrl.isEmpty())
                m_appsModel->setIconUrl(
                    name, iconUrl,
                    m_uiPluginManager->pluginManifestVersion(name));
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
    // Same reasoning as fetchUiPluginMetadata: no package_manager, no
    // dependency info to fetch, and no reason to block on discovering that.
    if (!moduleIsLoaded(m_coreModuleManager, "package_manager")) return;

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
            self->m_blockingDepsByModule.clear();
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
        auto blockingMap = std::make_shared<QMap<QString, QVariantList>>();
        auto dependenciesMap = std::make_shared<QMap<QString, QStringList>>();
        auto dependentsMap = std::make_shared<QMap<QString, QStringList>>();
        QPointer<PackageCoordinator> selfCopy(self.data());

        auto maybeFinish = [selfCopy, missingMap, blockingMap, dependenciesMap,
                            dependentsMap, remaining]() {
            if (!selfCopy) return;
            if (--(*remaining) > 0) return;
            selfCopy->m_missingDepsByModule = *missingMap;
            selfCopy->m_blockingDepsByModule = *blockingMap;
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
            // One call, three caches: the names the load gate refuses on, the
            // reason per name, and the on-disk closure the uninstall plan
            // walks. Why the dependency cleanup needs no extra IPC.
            inner.package_manager.resolveFlatDependenciesAsync(
                name, true,
                [missingMap, blockingMap, dependenciesMap, name,
                 maybeFinish](QVariantList deps) {
                    // Two questions, not one: which rows are on disk (the
                    // graph) and which refuse the load (the gate). The split
                    // lives in utils/DependencyBlocker.h so it is under test —
                    // this lambda is not reachable from one.
                    const logos::DependencyRowSplit split =
                        logos::splitDependencyRows(deps);
                    missingMap->insert(name, split.blocking);
                    blockingMap->insert(name, split.blockers);
                    dependenciesMap->insert(name, split.present);
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
    // Directional: a resolved version OLDER than the installed one is a
    // downgrade, and labelling it "upgrade" understates what is about to happen.
    return logos::semver::compare(resolvedVersion.toStdString(),
                                  installedVersion.toStdString()) >= 0
               ? QStringLiteral("upgrade")
               : QStringLiteral("downgrade");
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
        // The dialog renders `repository`; without it every dep row lost its
        // source label. Display name when the catalog knows one, else the URL.
        {QStringLiteral("repository"),    entry.value("repositoryDisplayName").toString().isEmpty()
                                              ? entry.value("repositoryUrl").toString()
                                              : entry.value("repositoryDisplayName").toString()},
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

    emit requiredPackagesResolved(requiredEntries);

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

    if (!m_logosAPI) {
        if (onDone) onDone(false, QStringLiteral("package_manager not connected"));
        return;
    }

    const bool alreadyInstalled = m_installedNameSet.contains(packageName);
    const bool isEmbedded =
        m_installTypeByModule.value(packageName) == QLatin1String("embedded");

    // Never tear down or remove our own UI — same guard uninstallUiModule
    // carries, for the same reason: it would brick Basecamp mid-install.
    // AppsFilterProxy::excludeMainUi only hides it from the list; it is not a
    // safety gate, and a resolver result can name it as a transitive entry.
    // Falling through installs over it, which is the old merge behaviour —
    // strictly better than deleting the running UI.
    const bool isSelf = (packageName == QStringLiteral("main_ui"));
    if (isSelf && alreadyInstalled) {
        qWarning() << "Refusing to remove main_ui before install; "
                      "installing over it instead";
    }

    if (alreadyInstalled && !isEmbedded && !isSelf) {
        cascadeUnloadForPackage(packageName);

        LogosModules logos(m_logosAPI);
        QPointer<PackageCoordinator> self(this);
        logos.package_manager.uninstallPackageAsync(packageName,
            [self, dl, packageName, onDone](QVariantMap uninstallResult) {
                if (!self) return;
                if (!uninstallResult.value("success", false).toBool()) {
                    const QString err = uninstallResult.value("error").toString();
                    qWarning() << "Pre-install removal of" << packageName
                               << "failed, aborting install:" << err;
                    if (onDone)
                        onDone(false, err.isEmpty()
                                   ? QStringLiteral("Could not remove the installed version")
                                   : err);
                    return;
                }
                self->installDownloadedFile(dl, onDone);
            });
        return;
    }

    installDownloadedFile(dl, onDone);
}

void PackageCoordinator::installDownloadedFile(const QVariantMap& dl,
    std::function<void(bool, const QString&)> onDone)
{
    const QString packageName = dl.value("name").toString();
    const QString filePath    = dl.value("path").toString();

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    // Installing was left on the default 20 s IPC deadline while DOWNLOADING
    // got five minutes -- backwards. Downloading is network-bound and can be
    // retried; installing is disk-bound over a payload that package_manager
    // reads, gunzips and tar-parses three times and Merkle-hashes twice, then
    // extracts and copies. It is ~1 s on a warm dev box and unbounded on a slow
    // disk, a large package, or a machine where the antivirus scans every DLL
    // as it lands. Blowing the deadline does not cancel any of that work: the
    // files still install and the reply is simply abandoned, so the user is
    // told the install failed when it did not. Match the download budget.
    constexpr int kInstallIpcDeadlineMs = 5 * 60 * 1000;
    // `installPluginAsyncResult`, not `installPluginAsync`: the plain async
    // wrapper hands the callback a bare QVariantMap, so a transport failure is
    // indistinguishable from a provider that legitimately returned an empty
    // one. AsyncResult<T> carries the value and the error together, which is
    // the whole reason it exists.
    logos.package_manager.installPluginAsyncResult(filePath, false,
        [self, packageName, onDone](logos::AsyncResult<QVariantMap> r) {
            if (!self) return;
            // Transport-level failure FIRST -- a timeout leaves `value`
            // default-constructed, and reading it as an install verdict is
            // exactly the mistake this channel removes. The message names the
            // module and the deadline (logos::callErrorTimeout builds it), and
            // says the package may in fact be installed: blowing the deadline
            // cancels nothing, so the files may well be on disk.
            if (!r.ok()) {
                const QString detail = QString::fromStdString(r.error.message);
                if (onDone) onDone(false,
                    QStringLiteral("%1 — the package may in fact be installed; check before retrying")
                        .arg(detail.isEmpty()
                                 ? QStringLiteral("package_manager did not reply")
                                 : detail));
                return;
            }
            const bool success = installPluginSucceeded(r.value);
            const QString err  = r.value.value("error").toString();
            if (onDone) onDone(success, success ? QString() : err);
        },
        Timeout(kInstallIpcDeadlineMs));
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
