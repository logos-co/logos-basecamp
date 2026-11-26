#include "PackageCoordinator.h"
#include "ResolverRequest.h"
#include "OptionalDependencyPreview.h"
#include "AppsModel.h"
#include "CoreModuleManager.h"
#include "UIPluginManager.h"
#include "LogosBasecampPaths.h"
#include "OnboardingState.h"
#include "RepositorySource.h"
#include "utils/BuiltInApps.h"
#include "utils/DependencyBlocker.h"
#include "utils/InstallTrust.h"
#include "utils/PublisherKeyring.h"
#include "ShellIntents.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QHash>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <memory>

#include "LogosIntent.h"

#include <logos/semver.hpp>

#include "logos_sdk.h"

PackageCoordinator::PackageCoordinator(LogosAPI* logosAPI,
                               CoreModuleManager* coreModuleManager,
                               UIPluginManager* uiPluginManager,
                               AppsModel* appsModel,
                               const LogosBasecamp::HostProfile& hostProfile,
                               QObject* parent)
    : QObject(parent)
    , m_hostProfile(hostProfile)
    , m_logosAPI(logosAPI)
    , m_coreModuleManager(coreModuleManager)
    , m_uiPluginManager(uiPluginManager)
    , m_appsModel(appsModel)
{
    logos::trust::PublisherKeyring::Keyring keyring;
    using Done = logos::trust::PublisherKeyring::Done;
    // Every keyring call names Basecamp's folder first: package_manager would
    // forget it if it were ever loaded again, and read another keyring.
    const QString keyringDir = QDir(LogosBasecampPaths::baseDirectory()).filePath(QStringLiteral("keyring"));
    const auto inKeyring = [logosAPI, keyringDir](std::function<void(bool)> call) {
        LogosModules(logosAPI).package_manager.setKeyringDirectoryAsyncResult(
            keyringDir, [call](logos::AsyncResult<void> r) { call(r.ok()); });
    };
    keyring.list = [logosAPI, inKeyring](std::function<void(bool, const QVariantList&)> then) {
        if (!logosAPI) { then(false, {}); return; }
        inKeyring([logosAPI, then](bool set) {
            if (!set) { then(false, {}); return; }
            LogosModules(logosAPI).package_manager.listTrustedKeysAsyncResult(
                [then](logos::AsyncResult<QVariantList> r) { then(r.ok(), r.value); });
        });
    };
    const auto answered = [](Done done) {
        return [done](QVariantMap result) {
            done(result.value(QStringLiteral("success")).toBool(),
                 result.value(QStringLiteral("error")).toString());
        };
    };
    const QString unavailable = QStringLiteral("package_manager is not available.");
    keyring.add = [logosAPI, inKeyring, answered, unavailable](const QVariantMap& key, Done done) {
        if (!logosAPI) { done(false, unavailable); return; }
        inKeyring([logosAPI, answered, unavailable, key, done](bool set) {
            if (!set) { done(false, unavailable); return; }
            LogosModules(logosAPI).package_manager.addTrustedKeyAsync(
                key.value(QStringLiteral("name")).toString(), key.value(QStringLiteral("did")).toString(),
                key.value(QStringLiteral("displayName")).toString(), key.value(QStringLiteral("url")).toString(),
                answered(done));
        });
    };
    keyring.remove = [logosAPI, inKeyring, answered, unavailable](const QString& name, Done done) {
        if (!logosAPI) { done(false, unavailable); return; }
        inKeyring([logosAPI, answered, unavailable, name, done](bool set) {
            if (!set) { done(false, unavailable); return; }
            LogosModules(logosAPI).package_manager.removeTrustedKeyAsync(name, answered(done));
        });
    };
    m_publisherKeyring = new logos::trust::PublisherKeyring(std::move(keyring), this);
    m_bundledNames = logos::trust::reservedNames(
        QDir(LogosBasecampPaths::embeddedModulesDirectory()).entryList(QDir::Dirs | QDir::NoDotAndDotDot),
        QDir(LogosBasecampPaths::embeddedPluginsDirectory()).entryList(QDir::Dirs | QDir::NoDotAndDotDot));
    connect(m_publisherKeyring, &logos::trust::PublisherKeyring::changed,
            this, &PackageCoordinator::trustedPublishersChanged);
    connect(m_publisherKeyring, &logos::trust::PublisherKeyring::changeFailed,
            this, &PackageCoordinator::trustedPublisherChangeFailed);

    // Only where the catalogue and the user's own install dir are: a dev host
    // has neither to set up.
    m_onboardingRequired = m_hostProfile.packageCatalog && m_hostProfile.useUserModules
        && OnboardingState::required(LogosBasecampPaths::baseDirectory());

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

LogosAPIClient* PackageCoordinator::downloaderClient() const
{
    if (!m_logosAPI || !moduleIsLoaded(m_coreModuleManager, "package_downloader"))
        return nullptr;
    return m_logosAPI->getClient("package_downloader");
}

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
    logos.package_manager.setEmbeddedUiPluginsDirectory(LogosBasecampPaths::embeddedPluginsDirectory());
    // The profile's roots go after the embedded dirs, so they win a name clash.
    for (const QString& dir : m_hostProfile.coreModuleRoots)
        logos.package_manager.addEmbeddedModulesDirectory(dir);
    for (const QString& dir : m_hostProfile.uiPluginRoots)
        logos.package_manager.addEmbeddedUiPluginsDirectory(dir);
    if (m_hostProfile.useUserModules) {
        logos.package_manager.setUserModulesDirectory(LogosBasecampPaths::modulesDirectory());
        logos.package_manager.setUserUiPluginsDirectory(LogosBasecampPaths::pluginsDirectory());
        configureTrust();
    }

    logos.package_manager.on("corePluginFileInstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "Core module file installed:" << data[0].toString();
        emit packageFilesChanged();
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
        emit packageFilesChanged();
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
        emit packageFilesChanged();
        QTimer::singleShot(100, this, [this]() {
            if (m_coreModuleManager) m_coreModuleManager->refresh();
            fetchUiPluginMetadata();
        });
    });

    logos.package_manager.on("uiPluginUninstalled", [this](const QVariantList& data) {
        if (data.isEmpty()) return;
        qDebug() << "UI plugin uninstalled:" << data[0].toString();
        emit packageFilesChanged();
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
        // Only worth a warning where the catalogue is expected.
        if (m_hostProfile.packageCatalog && !m_warnedPackageDownloaderMissing) {
            m_warnedPackageDownloaderMissing = true;
            qWarning() << "PackageCoordinator: package_downloader is not loaded -- skipping its "
                          "event subscriptions; this will be retried automatically.";
        }
        return;
    }
    m_packageDownloaderSubscribed = true;

    LogosModules logos(m_logosAPI);

    // Another consumer may start or stop it. A stop is for everyone: the next
    // operation here that needs it starts it again.
    logos.package_downloader.on("stateChanged", [this](const QVariantList& data) {
        m_downloaderStarted = data.value(0).toString() == QStringLiteral("running");
    });

    logos.package_downloader.on("catalogChanged", [this](const QVariantList&) {
        // Only what something has asked for: nothing reads the rest.
        if (m_repositoriesWanted) refreshRepositories();
        if (m_catalogWanted) refresh();
    });

    // Which transport served a package, `logos:<cid>` or the https url, for
    // whichever app asked for it. Payload is [packageName, source]. Logged
    // only: nothing shows it yet.
    logos.package_downloader.on("downloadDone", [this](const QVariantList& data) {
        if (data.size() < 2) {
            qWarning() << "PackageCoordinator: package_downloader.downloadDone "
                          "expected [name, source], got" << data.size() << "args";
            return;
        }
        qInfo() << "Downloaded" << data.at(0).toString() << "from" << data.at(1).toString();
        emit packageOnItsWay(data.at(0).toString());
    });
    // [packageName, received, total, transport], while its bytes are on the wire.
    logos.package_downloader.on("downloadProgress", [this](const QVariantList& data) {
        if (!data.isEmpty()) emit packageOnItsWay(data.at(0).toString());
    });

    // A reloaded instance starts stopped, and the next operation here starts it.
    // The status can arrive off the GUI thread.
    QPointer<PackageCoordinator> self(this);
    // Armed: storageStateChanged events sent before this are lost, so read the
    // state once now.
    logos.package_downloader.onSubscriptionStatus([self](logos::SubStatus status, std::uint64_t) {
        const bool armed = status == logos::SubStatus::Armed;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, armed]() {
            if (!self) return;
            if (armed) self->refreshStorageState();
            else self->m_downloaderStarted = false;
        }, Qt::QueuedConnection);
    });

    logos.package_downloader.on("storageStateChanged", [this](const QVariantList& data) {
        setStorageState(data.value(0).toString(), data.value(1).toString());
    });

    // Started as soon as it is loaded, as before it had start(), unless
    // onboarding has yet to choose the download source. The storage node comes
    // up with it unless that source is "http". After a stop in the Storage UI,
    // the next download that needs the node starts it.
    withDownloaderStarted([this]() {
        refreshDownloadSource();
        refreshStorageState();
    });
}

// The calls run whatever start() answers: a downloader that predates it refuses
// the method and is running already, and any other failure surfaces on the
// call itself.
void PackageCoordinator::withDownloaderStarted(std::function<void()> then)
{
    if (m_downloaderStarted) {
        then();
        return;
    }

    m_waitingForDownloaderStart.append(std::move(then));

    // chooseOnboardingSource() starts it.
    if (m_onboardingRequired && !m_onboardingSourceChosen) return;

    startDownloader();
}

void PackageCoordinator::startDownloader()
{
    if (m_downloaderStarting) return;  // a start is on its way
    m_downloaderStarting = true;

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_downloader.startAsyncResult([self](logos::AsyncResult<QVariantMap> r) {
        if (!self) return;
        self->m_downloaderStarting = false;
        self->m_downloaderStarted = r.ok() && r.value.value("success").toBool();
        if (!self->m_downloaderStarted) {
            qWarning() << "PackageCoordinator: package_downloader.start did not succeed:"
                       << (r.ok() ? r.value.value("error").toString()
                                  : QString::fromStdString(r.error.message));
        } else {
            self->requestStorage();
        }
        const QList<std::function<void()>> waiting =
            std::exchange(self->m_waitingForDownloaderStart, {});
        for (const auto& call : waiting) call();
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
    // A cancel goes through cancelPendingAction.
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

    // Emitted here rather than at each call site: whatever was just torn down
    // has left the loaded lists, and every caller needs them re-read.
    emit coreModulesChanged();
    emit uiModulesChanged();
    emit launcherAppsChanged();
}

void PackageCoordinator::confirmUninstallCascade(const QString& moduleName, const QStringList& optionalNames, const QVariantMap& optionalVersionPins, bool trustPublishers)
{
    if (m_pendingAction.op != PendingOp::UninstallCascade
        || m_pendingAction.name != moduleName) {
        qWarning() << "confirmUninstallCascade for" << moduleName
                   << "but pending action is" << m_pendingAction.name;
        return;
    }
    const QVariantMap answer = gateAnswer(moduleName, optionalNames, optionalVersionPins);
    const QString requestId = m_pendingAction.intentRequestId;
    if (!m_pendingAction.isUpgrade) {
        runUninstallCascade(moduleName, answer);
        return;
    }
    QPointer<PackageCoordinator> self(this);
    admitTrust(moduleName, requestId, trustPublishers,
               [self, moduleName, requestId, answer](const QString& refused) {
        if (!self || self->m_pendingAction.intentRequestId != requestId) return;
        if (!refused.isEmpty()) {
            self->endGatePreview(moduleName);
            self->m_pendingAction = {};
            self->refuseConfirmed(requestId, moduleName, refused);
            return;
        }
        self->runUninstallCascade(moduleName, answer);
    });
}

void PackageCoordinator::runUninstallCascade(const QString& moduleName, const QVariantMap& answer)
{
    const QString requestId = m_pendingAction.intentRequestId;
    const bool    isUpgrade = m_pendingAction.isUpgrade;
    endGatePreview(moduleName);
    m_pendingAction = {};

    // An uninstall's answer names what may be removed.
    QVariantMap approved = answer;
    if (!isUpgrade) approved.insert(QStringLiteral("names"), QStringList{moduleName});

    QPointer<PackageCoordinator> selfDefer(this);
    QMetaObject::invokeMethod(this, [this, selfDefer, moduleName, requestId, isUpgrade, approved]() {
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
        finishIntent(requestId, true, QString(), approved, [selfDefer, moduleName, isUpgrade](bool heard) {
            if (selfDefer && !heard && !isUpgrade) selfDefer->performLocalRemoval({moduleName});
        });

        emit coreModulesChanged();
        emit uiModulesChanged();
        emit launcherAppsChanged();
    }, Qt::QueuedConnection);  // run the cascade off the click stack
}

void PackageCoordinator::refresh()
{
    fetchUiPluginMetadata();
    if (m_repositoriesWanted) refreshRepositories();
}

void PackageCoordinator::requestCatalog()
{
    if (m_catalogWanted) return;
    m_catalogWanted = true;
    fetchCatalogNow();
}

void PackageCoordinator::whenCatalogLoaded(std::function<void()> then)
{
    if (m_catalogFetched) {
        then();
        return;
    }

    m_waitingForCatalog.append(std::move(then));
    m_catalogWanted = true;
    if (!m_catalogFetchInFlight) fetchCatalogNow();
}

void PackageCoordinator::fetchCatalogNow()
{
    if (!m_appsLoading) {
        m_appsLoading = true;
        emit appsLoadingChanged();
    }

    // The install offers label packages by repository.
    refreshRepositories();
    tryFetchCatalog(m_installedVersionByName, /*retriesLeft=*/10);
}

void PackageCoordinator::catalogFetchFinished()
{
    m_catalogFetched = true;
    m_catalogFetchInFlight = false;
    const QList<std::function<void()>> waiting = std::exchange(m_waitingForCatalog, {});
    for (const auto& then : waiting) then();
}

void PackageCoordinator::cancelPendingAction(const QString& moduleName)
{
    if (m_pendingAction.op != PendingOp::UninstallCascade
        || m_pendingAction.name != moduleName) {
        // MainUIBackend fans out cancelPendingAction to both managers so one
        // of them is always a no-op — don't even warn here.
        return;
    }
    endGatePreview(moduleName);
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

void PackageCoordinator::finishIntent(const QString& requestId, bool ok, const QString& error,
                                      const QVariant& data, std::function<void(bool)> heard)
{
    if (requestId.isEmpty() || !m_intentResponder) {
        if (heard) heard(false);
        return;
    }
    m_intentResponder(requestId, ok, error, data, std::move(heard));
}

// Only the dialog previewing `name` may end its preview; unrelated dialogs must
// not cancel an in-flight gate resolution.
void PackageCoordinator::endGatePreview(const QString& name)
{
    if (name.isEmpty() || name != m_pendingPreviewName) return;
    ++m_gatePreviewEpoch;
    m_pendingPreviewName.clear();
    m_pendingOptionalPackages.clear();
}

QVariantMap PackageCoordinator::selectedOptionalPackages(const QStringList& names,
                                                         const QVariantMap& versionPins) const
{
    // Only real changes: re-requesting an installed release would reinstall it.
    QVariantList requests;
    for (const QVariant& v : m_pendingOptionalPackages) {
        const QVariantMap offer = v.toMap();
        for (const QVariant& r : logos::selectedOptionalRequests({offer}, names, versionPins))
            if (!logos::keepsInstalledRelease(offer, r.toMap())) requests.append(r);
    }
    return {{QStringLiteral("optionalPackages"), requests}};
}

QVariantMap PackageCoordinator::optionalPackageRow(const QVariantMap& offer) const
{
    QVariantMap row = offer;
    const QString name = offer.value("name").toString();
    const QString repo = offer.value("repositoryUrl").toString();
    const QVariantMap catalogRow = m_appsModel ? m_appsModel->rowDataByName(name, repo) : QVariantMap{};
    row.insert("optional", true);
    if (offer.contains("error")) row.insert("resolverError", offer.value("error"));
    // An installed-only offer has no catalog row; its installed manifest describes it.
    QVariantMap installed;
    if (catalogRow.isEmpty()) {
        for (const QVariant& v : m_installedPackagesCache) {
            const QVariantMap pkg = v.toMap();
            const QString module = pkg.value("moduleName").toString();
            if ((module.isEmpty() ? pkg.value("name").toString() : module) == name) { installed = pkg; break; }
        }
    }
    const QString displayName = catalogRow.value("displayName").toString();
    row.insert("displayName", !displayName.isEmpty() ? displayName
                                                     : m_displayNameByModule.value(name, name));
    row.insert("description", catalogRow.isEmpty() ? installed.value("description")
                                                    : catalogRow.value("description"));
    row.insert("toVersion", offer.value("version"));
    // The requester's pick, which the dialog starts at.
    const QString requested = m_pendingPreviewChoices.optionalPins.value(name).toString();
    if (!requested.isEmpty()) row.insert("requestedVersion", requested);
    row.insert("action", offer.contains("error") ? QStringLiteral("error")
        : depAction(m_installedVersionByName.value(name), offer.value("version").toString(),
                    m_installedHashByName.value(name), offer.value("rootHash").toString()));
    QVariantList versions;
    for (const QVariant& v : offer.value("versions").toList()) {
        QVariantMap version = v.toMap();
        version.insert("action", depAction(m_installedVersionByName.value(name),
            version.value("manifest").toMap().value("version").toString(),
            m_installedHashByName.value(name), version.value("rootHash").toString()));
        versions.append(version);
    }
    row.insert("versions", versions);
    return row;
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
                                               bool localFile,
                                               bool newestReleases,
                                               const logos::InstallChoices& choices,
                                               std::function<void(bool, const QVariantList&)> then)
{
    // Nothing to resolve against (a local .lgx), or no downloader. Answer
    // immediately rather than leaving the dialog unopened — but as UNRESOLVED,
    // so it says the dependencies are unknown instead of claiming there are
    // none.
    if (localFile || !m_logosAPI) {
        then(false, {});
        return;
    }

    m_pendingPreviewName = name;
    m_pendingPreviewRepo = repositoryUrl;
    m_pendingPreviewNewest = newestReleases;
    m_pendingPreviewChoices = choices;
    const int epoch = ++m_gatePreviewEpoch;
    QPointer<PackageCoordinator> self(this);
    LogosModules logos(m_logosAPI);
    logos.package_manager.getInstalledPackagesAsync(
        [self, name, repositoryUrl, newestReleases, choices, epoch, then](QVariantList installed) {
        if (!self || self->m_gatePreviewEpoch != epoch) return;
        self->resolveOptionalPreview(name, repositoryUrl, choices.pins, choices.selection, choices.optionalPins,
            newestReleases ? QString() : logos::installedPackagesJson(installed),
            [self, epoch]() { return self && self->m_gatePreviewEpoch == epoch; },
            [self, name, then](QVariantList resolved) {
                if (!self) return;
                then(!resolved.isEmpty(), self->gatePreviewChanges(resolved));
                if (self) self->assessPlanTrust(name, resolved);
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

void PackageCoordinator::beginPackageConfirmation(const QString& dispatchId,
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
        return;   // answered — do NOT let the broker time it out too
    }

    const QString name    = params.value(QStringLiteral("name")).toString();
    const QString version = params.value(QStringLiteral("version")).toString();
    m_pendingOptionalPackages.clear();
    m_pendingPreviewName.clear();
    ++m_gatePreviewEpoch;

    if (intent == QLatin1String("basecamp.packages.confirm_install")) {
        if (name.isEmpty()) {
            finishIntent(dispatchId, false, logos::intent::errBadRequest());
            return;
        }
        // A fresh install unloads nothing, so there is no cascade and no
        // PendingAction — the dialog's answer is the whole flow.
        m_pendingInstallRequestId = dispatchId;
        m_pendingInstallName      = name;
        const QString repositoryUrl = params.value(QStringLiteral("repositoryUrl")).toString();
        const bool localFile = logos::trust::requestsLocalFile(params);
        beginConfirmationTrust(name, version, localFile, params, requesterName);
        resolveDepChangesThen(name, repositoryUrl, localFile, installsNewestReleases(requesterName),
                              logos::installChoicesFrom(params, name, version),
            [this, name, version, requesterName, dispatchId](bool resolved,
                                                                      const QVariantList& changes) {
                emit installGateConfirmationRequested(name, version, changes, requesterName,
                                                     requesterIsBundled(requesterName),
                                                     resolved, dispatchId);
                // The dialog opens on what is known so far; the rest follows.
                if (m_gateTrust.name == name) emitTrust();
            });
        return;
    }

    if (intent == QLatin1String("basecamp.packages.confirm_upgrade")) {
        if (name.isEmpty()) {
            finishIntent(dispatchId, false, logos::intent::errBadRequest());
            return;
        }
        const int mode = params.value(QStringLiteral("mode")).toInt();
        const QStringList installedDeps = dependentsOfBatch({name});
        const QStringList loadedDeps = m_uiPluginManager
            ? m_uiPluginManager->intersectWithLoaded(installedDeps)
            : QStringList{};
        m_pendingAction = {PendingOp::UninstallCascade, name, {}, dispatchId,
                           /*isUpgrade=*/true};
        const QString repositoryUrl = params.value(QStringLiteral("repositoryUrl")).toString();
        const bool localFile = logos::trust::requestsLocalFile(params);
        beginConfirmationTrust(name, version, localFile, params, requesterName);
        resolveDepChangesThen(name, repositoryUrl, localFile, installsNewestReleases(requesterName),
                              logos::installChoicesFrom(params, name, version),
            [this, name, version, mode, installedDeps, loadedDeps, requesterName, dispatchId]
            (bool /*resolved*/, const QVariantList& changes) {
                emit upgradeCascadeConfirmationRequested(name, version, mode, installedDeps,
                                                        loadedDeps, changes, requesterName,
                                                        requesterIsBundled(requesterName), dispatchId);
                if (m_gateTrust.name == name) emitTrust();
            });
        return;
    }

    if (intent == QLatin1String("basecamp.packages.confirm_uninstall")) {
        QStringList names;
        for (const QVariant& v : params.value(QStringLiteral("names")).toList()) {
            const QString n = v.toString();
            if (!n.isEmpty() && !names.contains(n)) names.append(n);
        }
        if (names.isEmpty()) {
            finishIntent(dispatchId, false, logos::intent::errBadRequest());
            return;
        }

        // Held at once, so a cancel or a withdrawal finds it. The plan is read
        // off the installed packages: until they are known, the dialog waits.
        m_pendingAction = {PendingOp::UninstallCascade, names.first(), {}, dispatchId};
        if (m_dependencyDataReady || !m_logosAPI) {
            planUninstall(dispatchId, names, requesterName);
            return;
        }
        QVariantMap loading = buildLoadingPayload(
            names.first(), removesAnApp(names, requesterName) ? QStringLiteral("app") : QStringLiteral("packages"));
        loading.insert(QStringLiteral("requestId"), dispatchId);
        emit uninstallPlanRequested(loading);
        auto once = std::make_shared<QMetaObject::Connection>();
        QPointer<PackageCoordinator> self(this);
        *once = connect(this, &PackageCoordinator::dependencyDataReadyChanged, this,
                        [self, once, dispatchId, names, requesterName]() {
            QObject::disconnect(*once);
            if (self && self->m_pendingAction.intentRequestId == dispatchId)
                self->planUninstall(dispatchId, names, requesterName);
        });
        return;
    }

    qWarning() << "PackageCoordinator: unhandled package intent" << intent;
    finishIntent(dispatchId, false, logos::intent::errFailed());
}

bool PackageCoordinator::installsNewestReleases(const QString& requesterName)
{
    const auto* defaultApp = logos::defaultapps::appNamed(requesterName);
    return defaultApp && defaultApp->slot == logos::defaultapps::Slot::Apps;
}

bool PackageCoordinator::removesAnApp(const QStringList& names, const QString& requesterName)
{
    const auto* defaultApp = logos::defaultapps::appNamed(requesterName);
    return names.size() == 1 && defaultApp && defaultApp->slot == logos::defaultapps::Slot::Apps;
}

void PackageCoordinator::planUninstall(const QString& dispatchId, const QStringList& names,
                                       const QString& requesterName)
{
    // Built in, or Basecamp's own: nothing removes it, and the cascade would
    // unload it first.
    const QStringList irremovable = uninstallplan::irremovable(planInput(names));
    if (!irremovable.isEmpty()) {
        qWarning() << "PackageCoordinator:" << irremovable << "can't be removed — refusing confirm_uninstall";
        m_pendingAction = {};
        emit confirmationWithdrawn(dispatchId);
        finishIntent(dispatchId, false, logos::intent::errBadRequest());
        return;
    }
    // An app the `apps` slot removes takes with it what only that app needed.
    const bool app = removesAnApp(names, requesterName);
    const QStringList batch = app ? uninstallplan::composeFrom(planInput(names)).batch : names;
    if (app) m_lastRequestedTargets = names;
    const bool multi = batch.size() > 1;
    QVariantMap plan = buildPlanPayload(batch, dependentsOfBatch(batch),
                                        app ? QStringLiteral("app") : QStringLiteral("packages"),
                                        multi, requesterName);
    m_lastRequestedTargets.clear();
    if (batch.isEmpty() || plan.value(QStringLiteral("removable")).toList().isEmpty()) {
        qWarning() << "PackageCoordinator: nothing removable in" << names << "— refusing confirm_uninstall";
        m_pendingAction = {};
        emit confirmationWithdrawn(dispatchId);
        finishIntent(dispatchId, false, logos::intent::errBadRequest());
        return;
    }
    m_pendingAction = multi
        ? PendingAction{PendingOp::MultiUninstallCascade, {}, batch, dispatchId}
        : PendingAction{PendingOp::UninstallCascade, batch.first(), {}, dispatchId};
    plan.insert(QStringLiteral("requestId"), dispatchId);
    emit uninstallPlanRequested(plan);
}

// A fresh install unloads nothing, so both of these are just the answer — but
// only for the package the pending dialog actually names.
void PackageCoordinator::confirmInstallGate(const QString& name, const QStringList& optionalNames, const QVariantMap& optionalVersionPins, bool trustPublishers)
{
    if (m_pendingInstallName != name) {
        qWarning() << "confirmInstallGate for" << name
                   << "but pending install is" << m_pendingInstallName;
        return;
    }
    const QString requestId = m_pendingInstallRequestId;
    const QVariantMap answer = gateAnswer(name, optionalNames, optionalVersionPins);
    QPointer<PackageCoordinator> self(this);
    admitTrust(name, requestId, trustPublishers,
               [self, name, requestId, answer](const QString& refused) {
        if (!self || self->m_pendingInstallRequestId != requestId) return;
        self->m_pendingInstallRequestId.clear();
        self->m_pendingInstallName.clear();
        self->endGatePreview(name);
        if (!refused.isEmpty()) {
            self->refuseConfirmed(requestId, name, refused);
            return;
        }
        self->finishIntent(requestId, true, QString(), answer);
    });
}

QVariantMap PackageCoordinator::gateAnswer(const QString& name, const QStringList& optionalNames,
                                           const QVariantMap& optionalVersionPins) const
{
    QVariantMap answer = name == m_pendingPreviewName
        ? selectedOptionalPackages(optionalNames, optionalVersionPins) : QVariantMap{};
    if (m_gateTrust.name == name && !m_gateTrust.about.isEmpty()
        && m_gateTrust.about == gateDispatchId(name))
        answer.insert(m_gateTrust.approval);
    return answer;
}

void PackageCoordinator::refuseConfirmed(const QString& requestId, const QString& name,
                                         const QString& reason)
{
    qWarning() << "PackageCoordinator: not installing" << name << ":" << reason;
    finishIntent(requestId, false, logos::intent::errFailed());
    emit installRefused(name, reason);
}

void PackageCoordinator::cancelInstallGate(const QString& name)
{
    if (m_pendingInstallName != name) return;
    const QString requestId = m_pendingInstallRequestId;
    m_pendingInstallRequestId.clear();
    m_pendingInstallName.clear();
    endGatePreview(name);
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
    QPointer<PackageCoordinator> self(this);
    finishIntent(requestId, true, QString(), QVariantMap{{QStringLiteral("names"), moduleNames}},
                 [self, moduleNames](bool heard) {
        if (self && !heard) self->performLocalRemoval(moduleNames);
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
    const QString requestId = m_pendingAction.intentRequestId;
    m_pendingAction = {};
    finishIntent(requestId, false, logos::intent::errCancelled());
}

// ---------------------------------------------------------------------------
// Metadata refresh chain
// ---------------------------------------------------------------------------

void PackageCoordinator::fetchUiPluginMetadata(bool afterSetAside)
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
    logos.package_manager.getInstalledUiPluginsAsync([self, afterSetAside](QVariantList uiPlugins) {
        if (!self) return;

        // Basecamp's own apps run only from its bundle (utils/BuiltInApps.h).
        // One that stays listed from elsewhere is refused when it would load.
        if (self->m_hostProfile.useUserModules) {
            bool moved = false;
            for (const auto& found : logos::builtin::copiesInApps(uiPlugins, LogosBasecampPaths::pluginsDirectory())) {
                const auto copy = afterSetAside
                    ? found : logos::builtin::setAside(found, LogosBasecampPaths::setAsideDirectory());
                moved = moved || !copy.to.isEmpty();
                emit self->builtInCopySetAside(copy.name, copy.from, copy.to, copy.loaded);
            }
            if (moved) {
                self->fetchUiPluginMetadata(/*afterSetAside=*/true);
                return;
            }
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
        self->fetchValidVariants();

        // The catalog follows once something has asked for it.
        if (self->m_catalogWanted)
            self->tryFetchCatalog(installedByName, /*retriesLeft=*/10);
    });
}

void PackageCoordinator::fetchValidVariants()
{
    if (!m_logosAPI || !m_appsModel) return;
    if (!moduleIsLoaded(m_coreModuleManager, "package_manager")) return;

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.getValidVariantsAsync([self](QVariant result) {
        if (!self || !self->m_appsModel) return;
        self->m_appsModel->setValidVariants(result.toStringList());
    });
}

void PackageCoordinator::tryFetchCatalog(const QHash<QString, QString>& installedByName, int retriesLeft)
{
    m_catalogFetchInFlight = true;

    LogosAPIClient* dlClient = downloaderClient();

    if (dlClient && dlClient->isConnected()) {
        withDownloaderStarted([this, dlClient, installedByName]() {
            QPointer<PackageCoordinator> self(this);
            dlClient->invokeRemoteMethodAsync(
                "package_downloader", "getCatalog", QVariantList{},
                [self, installedByName](QVariant catalogVar) {
                    if (!self) return;
                    const QVariantList catalog = catalogVar.toList();
                    self->buildCatalogIndexes(catalog);
                    self->populateAppsModel(catalog, installedByName);
                    self->catalogFetchFinished();
                });
        });
        return;
    }

    if (retriesLeft <= 0) {
        // Give up — drop the loading state so it doesn't stick.
        if (m_appsLoading) {
            m_appsLoading = false;
            emit appsLoadingChanged();
        }
        catalogFetchFinished();
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

// Stamp each repository with `displayLabel`, the one string every view
// should show for it
static QVariantList withDisplayLabels(const QVariantList& repos)
{
    // Comparison keys only — the rendered label keeps the repo's own
    // spelling. Trimmed and case-folded so neither a stray space nor a
    // different capitalisation of the same GitHub account (which GitHub
    // treats as one account) can slip past the collision check and leave
    // two repos rendering the same bare name.
    auto nameOf = [](const QVariantMap& r) {
        return r.value("displayName").toString().trimmed().toLower();
    };
    auto pairOf = [&](const QVariantMap& r) {
        return nameOf(r) + QChar(0x01)
             + r.value("sourceOwner").toString().trimmed().toLower();
    };

    // Counts first: a label depends on how many OTHER repos contest the name.
    QHash<QString, int> byName, byNameAndOwner;
    for (const QVariant& v : repos) {
        const QVariantMap r = v.toMap();
        byName[nameOf(r)]++;
        byNameAndOwner[pairOf(r)]++;
    }

    QVariantList out;
    out.reserve(repos.size());
    for (const QVariant& v : repos) {
        QVariantMap r = v.toMap();
        // Trimmed to match the collision keys above; a trailing space is
        // invisible on screen but would otherwise dodge the check.
        QString label = r.value("displayName").toString().trimmed();
        if (label.isEmpty()) label = r.value("name").toString().trimmed();

        const QString owner = r.value("sourceOwner").toString();
        const QString repo  = r.value("sourceRepo").toString();
        const QString host  = r.value("sourceHost").toString();
        QString shortSource = owner.isEmpty() ? host : owner;
        QString longSource  = (owner.isEmpty() || repo.isEmpty())
                                  ? shortSource
                                  : owner + QLatin1Char('/') + repo;
        if (shortSource.isEmpty()) shortSource = r.value("url").toString();
        if (longSource.isEmpty())  longSource  = shortSource;

        if (label.isEmpty()) {
            label = longSource;                    // nothing to qualify
        } else if (byName.value(nameOf(r)) > 1 && !shortSource.isEmpty()) {
            label = QCoreApplication::translate("PackageCoordinator", "%1 (%2)")
                        .arg(label, byNameAndOwner.value(pairOf(r)) > 1
                                        ? longSource : shortSource);
        }
        r.insert(QStringLiteral("displayLabel"), label);
        out.append(r);
    }
    return out;
}

QVariantMap PackageCoordinator::repositoryRow(const QString& url) const
{
    for (const QVariant& v : m_repositories) {
        const QVariantMap r = v.toMap();
        if (r.value(QStringLiteral("url")).toString() == url)
            return r;
    }
    return {};
}

QVariantMap PackageCoordinator::repositorySource(const QString& repositoryUrl) const
{
    const QVariantMap row = repositoryRow(repositoryUrl);
    if (row.isEmpty()) {
        // Not a repository we hold: removed since the catalog was fetched, or a
        // package that predates the registry. Report the address it names
        // without claiming to know whose it is. Empty in, empty out — the
        // dialogs hide the source line rather than render "from nowhere".
        return QVariantMap{{QStringLiteral("label"), repositoryUrl},
                           {QStringLiteral("link"),  repositoryUrl}};
    }

    const RepositorySource src = repositorySourceFor(row);
    return QVariantMap{{QStringLiteral("label"), src.label},
                       {QStringLiteral("link"),  src.link}};
}

QString PackageCoordinator::repositoryLabelFor(const QVariantMap& catalogEntry) const
{
    return repositoryLabelForEntry(
        catalogEntry,
        repositoryRow(catalogEntry.value(QStringLiteral("repositoryUrl")).toString()));
}

void PackageCoordinator::refreshRepositories()
{
    // Whoever asked shows them, so every refresh after this one follows.
    m_repositoriesWanted = true;

    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient || !dlClient->isConnected()) return;

    const bool wasLoading = m_repositoriesLoadingCount > 0;
    ++m_repositoriesLoadingCount;
    if (!wasLoading) emit repositoriesLoadingChanged();

    withDownloaderStarted([this, dlClient]() {
        QPointer<PackageCoordinator> self(this);
        dlClient->invokeRemoteMethodAsync(
            "package_downloader", "listRepositories", QVariantList{},
            [self](QVariant result) {
                if (!self) return;
                self->m_repositories = withDisplayLabels(result.toList());
                const int remaining = --self->m_repositoriesLoadingCount;
                emit self->repositoriesChanged();
                if (remaining == 0) emit self->repositoriesLoadingChanged();
            });
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
    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient || !dlClient->isConnected()) {
        emit repositoryOperationCompleted(QStringLiteral("add"), url, false,
            QStringLiteral("package_downloader not connected"));
        return;
    }
    withDownloaderStarted([this, dlClient, url]() {
        invokeRepositoryMutation(this, dlClient, QStringLiteral("addRepository"),
                                 QStringLiteral("add"), url, QVariantList{url});
    });
}

void PackageCoordinator::removeRepository(const QString& url)
{
    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient || !dlClient->isConnected()) {
        emit repositoryOperationCompleted(QStringLiteral("remove"), url, false,
            QStringLiteral("package_downloader not connected"));
        return;
    }
    withDownloaderStarted([this, dlClient, url]() {
        invokeRepositoryMutation(this, dlClient, QStringLiteral("removeRepository"),
                                 QStringLiteral("remove"), url, QVariantList{url});
    });
}

void PackageCoordinator::setRepositoryEnabled(const QString& url, bool enabled)
{
    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient || !dlClient->isConnected()) {
        emit repositoryOperationCompleted(QStringLiteral("setEnabled"), url, false,
            QStringLiteral("package_downloader not connected"));
        return;
    }
    withDownloaderStarted([this, dlClient, url, enabled]() {
        invokeRepositoryMutation(this, dlClient, QStringLiteral("setRepositoryEnabled"),
                                 QStringLiteral("setEnabled"), url,
                                 QVariantList{url, enabled});
    });
}

void PackageCoordinator::refreshDownloadSource()
{
    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient || !dlClient->isConnected()) return;

    withDownloaderStarted([this, dlClient]() {
        QPointer<PackageCoordinator> self(this);
        dlClient->invokeRemoteMethodAsync(
            "package_downloader", "getDownloadSource", QVariantList{},
            [self](QVariant result) {
                if (!self) return;
                // A downloader without the method answers nothing: stays empty.
                const QString source = result.toString();
                if (source == QStringLiteral("http")) self->releaseStorageIfUnused();
                if (source == self->m_downloadSource) return;
                self->m_downloadSource = source;
                emit self->downloadSourceChanged();
            });
    });
}

// The catalog refresh follows from the catalogChanged the downloader emits.
void PackageCoordinator::setDownloadSource(const QString& source)
{
    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient || !dlClient->isConnected()) {
        emit repositoryOperationCompleted(QStringLiteral("setDownloadSource"), source, false,
            QStringLiteral("package_downloader not connected"));
        return;
    }

    // Back from "http": the downloader starts the node once storage_module is
    // ready again.
    if (source != QStringLiteral("http") && m_coreModuleManager
        && !moduleIsLoaded(m_coreModuleManager, "storage_module")) {
        m_coreModuleManager->loadModule(QStringLiteral("storage_module"));
    }

    withDownloaderStarted([this, dlClient, source]() {
        QPointer<PackageCoordinator> self(this);
        dlClient->invokeRemoteMethodAsync(
            "package_downloader", "setDownloadSource", QVariantList{source},
            [self, source](QVariant result) {
                if (!self) return;
                const QVariantMap r = result.toMap();
                const bool ok = r.value("success").toBool();
                QString error = r.value("error").toString();
                if (!ok && error.isEmpty())
                    error = QStringLiteral("package_downloader did not accept the download source");
                emit self->repositoryOperationCompleted(QStringLiteral("setDownloadSource"),
                                                        source, ok, error);
                if (ok && source != QStringLiteral("http")) self->requestStorage();
                // Either way: a refused value must not stay selected.
                self->refreshDownloadSource();
            });
    });
}

void PackageCoordinator::completeOnboarding()
{
    if (!m_onboardingRequired) return;

    OnboardingState::markCompleted(LogosBasecampPaths::baseDirectory());
    m_onboardingRequired = false;
    emit onboardingRequiredChanged();
}

// Onboarding can go back and choose again: only the first choice starts the
// downloader, later ones change its source like Settings does.
void PackageCoordinator::chooseOnboardingSource(const QString& source)
{
    if (!m_onboardingRequired || m_onboardingSourceChosen) {
        setDownloadSource(source);
        return;
    }
    m_onboardingSourceChosen = true;

    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient) {
        if (!m_waitingForDownloaderStart.isEmpty()) startDownloader();
        return;
    }

    // Before start(), which saves it: under "http" the node then never comes
    // up. A downloader that predates that refuses it, and gets it once started.
    QPointer<PackageCoordinator> self(this);
    dlClient->invokeRemoteMethodAsync(
        "package_downloader", "setDownloadSource", QVariantList{source},
        [self, source](QVariant result) {
            if (!self) return;
            const bool saved = result.toMap().value("success").toBool();
            self->withDownloaderStarted([self, source, saved]() {
                if (!self) return;
                if (saved) self->refreshDownloadSource();
                else self->setDownloadSource(source);
                self->refreshStorageState();
            });
        });
}

// Not checked: under "http" it is refused, and a downloader that predates it
// starts the node itself.
void PackageCoordinator::requestStorage()
{
    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient) return;
    dlClient->invokeRemoteMethodAsync("package_downloader", "startStorage", QVariantList{},
                                      [](QVariant) {});
}

void PackageCoordinator::retryStorage()
{
    if (!m_coreModuleManager) return;

    if (moduleIsLoaded(m_coreModuleManager, "storage_module"))
        m_coreModuleManager->unloadModule(QStringLiteral("storage_module"));
    m_coreModuleManager->loadModule(QStringLiteral("storage_module"));

    // A restart, not just the reload: a start that never answered holds the
    // downloader in "starting", and it waits that out rather than start twice.
    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient) return;

    QPointer<PackageCoordinator> self(this);
    dlClient->invokeRemoteMethodAsync(
        "package_downloader", "stop", QVariantList{},
        [self](QVariant) {
            if (!self) return;
            self->m_downloaderStarted = false;
            self->withDownloaderStarted([self]() {
                if (self) self->refreshStorageState();
            });
        });
}

void PackageCoordinator::refreshStorageState()
{
    LogosAPIClient* dlClient = downloaderClient();
    if (!dlClient) return;

    QPointer<PackageCoordinator> self(this);
    dlClient->invokeRemoteMethodAsync(
        "package_downloader", "getStorageState", QVariantList{},
        [self](QVariant result) {
            if (!self) return;
            // A downloader without the method answers nothing: stays empty.
            const QVariantMap r = result.toMap();
            if (r.isEmpty()) return;
            const QString state = r.value("state").toString();
            self->setStorageState(state, r.value("error").toString());

            // Asked again until it settles, in case an event is lost.
            if (state == QStringLiteral("waiting") || state == QStringLiteral("starting")) {
                QTimer::singleShot(2000, self, [self]() {
                    if (self) self->refreshStorageState();
                });
            }
        });
}

void PackageCoordinator::setStorageState(const QString& state, const QString& error)
{
    if (state == m_storageState && error == m_storageError) return;
    m_storageState = state;
    m_storageError = error;
    emit storageStateChanged();
}

void PackageCoordinator::releaseStorageIfUnused()
{
    if (!m_coreModuleManager || !moduleIsLoaded(m_coreModuleManager, "storage_module"))
        return;

    // The downloader is the user being released from; anything else loaded
    // that depends on storage keeps it.
    const QStringList loaded = m_coreModuleManager->loadedModules();
    QStringList users = m_dependentsByModule.value(QStringLiteral("storage_module"));
    users.append(QStringLiteral("storage_ui"));
    users.removeAll(QStringLiteral("package_downloader"));
    for (const QString& user : users) {
        if (loaded.contains(user)) return;
    }
    if (m_uiPluginManager && !m_uiPluginManager->intersectWithLoaded(users).isEmpty())
        return;

    qInfo() << "PackageCoordinator: downloads are GitHub only and nothing else uses "
               "storage_module -- unloading it";
    m_coreModuleManager->unloadModule(QStringLiteral("storage_module"));
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
        emit self->installedPackagesRead(packages);
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
            // moduleName is the key openApp uses; fall back to name when the
            // field is absent.
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
                [missingMap, blockingMap, dependenciesMap, name, maybeFinish,
                 enforceVersions = self->m_hostProfile.enforceDependencyVersions](QVariantList deps) {
                    // Two questions, not one: which rows are on disk (the
                    // graph) and which refuse the load (the gate). The split
                    // lives in utils/DependencyBlocker.h so it is under test —
                    // this lambda is not reachable from one.
                    const logos::DependencyRowSplit split =
                        logos::splitDependencyRows(deps, enforceVersions);
                    for (const QVariant& m : split.ignoredMismatches) {
                        const QVariantMap row = m.toMap();
                        qWarning().noquote()
                            << name << ": dependency" << row.value("name").toString()
                            << row.value("detail").toString()
                            << "— loading anyway (dependency versions not enforced)";
                    }
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



QVariantMap nameAndRepo(const QString& name, const QString& repo)
{
    return {
        {QStringLiteral("name"),          name},
        {QStringLiteral("repositoryUrl"), repo},
    };
}

bool PackageCoordinator::alreadyOnDisk(const QString& installedVersion,
                                       const QString& installedHash,
                                       const QString& resolvedVersion,
                                       const QString& resolvedHash)
{
    if (installedVersion.isEmpty()) return false;
    const bool versionMatches =
        resolvedVersion.isEmpty() || resolvedVersion == installedVersion;
    const bool hashMatches =
        resolvedHash.isEmpty() || installedHash.isEmpty()
        || resolvedHash == installedHash;
    return versionMatches && hashMatches;
}

QString PackageCoordinator::depAction(const QString& installedVersion,
                                      const QString& resolvedVersion,
                                      const QString& installedHash,
                                      const QString& resolvedHash)
{
    if (installedVersion.isEmpty()) return QStringLiteral("install");
    // The same predicate the plan is built from.
    if (alreadyOnDisk(installedVersion, installedHash, resolvedVersion, resolvedHash))
        return QStringLiteral("installed");
    // Same version, so the hash is what differs: re-signed, re-published, or
    // drifted on disk.
    if (installedVersion == resolvedVersion)
        return QStringLiteral("reinstall");
    // Directional: a resolved version OLDER than the installed one is a
    // downgrade, and labelling it "upgrade" understates what is about to happen.
    return logos::semver::compare(resolvedVersion.toStdString(),
                                  installedVersion.toStdString()) >= 0
               ? QStringLiteral("upgrade")
               : QStringLiteral("downgrade");
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
        // NOT `repository` — the dialog renders that, but naming a repository
        // needs the registry and this is static. Both callers stamp it with
        // repositoryLabelFor().
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
        c.insert(QStringLiteral("repository"), repositoryLabelFor(m));
        const QString repoUrl = c.value("repositoryUrl").toString();
        c.insert(QStringLiteral("versions"),
                 m_versionsByRepoAndName.value(catalogKey(repoUrl, name)));
        c.insert("optional", m.value("optional", false));
        if (m.contains("requiredFor")) c.insert("requiredFor", m.value("requiredFor"));
        if (c.value("isTopLevel").toBool()) out.prepend(c);
        else                                out.append(c);
    }
    return out;
}

void PackageCoordinator::openApp(const QString& name,
                                 const QString& repositoryUrl,
                                 const QVariantMap& versionPins,
                                 bool allowFastLaunch)
{
    if (!m_logosAPI || name.isEmpty()) return;

    if (allowFastLaunch && fastLaunch(name, repositoryUrl)) return;

    // A bundled app has no catalog row before the catalog is fetched, so the
    // fast path gets a second look. Anything else is App Manager's to show.
    whenCatalogLoaded([this, name, repositoryUrl, allowFastLaunch]() {
        if (allowFastLaunch && fastLaunch(name, repositoryUrl)) return;
        emit appManagerRequested(name);
    });
}

bool PackageCoordinator::fastLaunch(const QString& name, const QString& repositoryUrl)
{
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
    if (tileStatus != InstallStatus::Installed) return false;

    qDebug() << "openApp fast-path: installed (v="
             << m_installedVersionByName.value(name)
             << "), emitting launchAppRequested";
    emit launchAppRequested(name);
    return true;
}

void PackageCoordinator::resolveOptionalPreview(const QString& name, const QString& repositoryUrl,
                                                const QVariantMap& versionPins,
                                                const QVariantMap& selection,
                                                const QVariantMap& optionalPins,
                                                const QString& installedJson,
                                                std::function<bool()> current,
                                                std::function<void(QVariantList)> then,
                                                bool selectNew)
{
    auto preview = std::make_shared<logos::OptionalDependencyPreview>(
        name, repositoryUrl, versionPins, selection, optionalPins);
    QMap<QString, QPair<QString, QString>> installed;
    for (auto it = m_installedVersionByName.cbegin(); it != m_installedVersionByName.cend(); ++it)
        installed.insert(it.key(), {it.value(), m_installedHashByName.value(it.key())});
    preview->setInstalled(installed);
    preview->setSelectNew(selectNew);
    withDownloaderStarted([this, preview, installedJson, current, then]() {
        resolveOptionalPreviewPass(preview, preview->initialRequest(), installedJson, {}, current, then);
    });
}

void PackageCoordinator::resolveOptionalPreviewPass(
    std::shared_ptr<logos::OptionalDependencyPreview> preview, const QString& request,
    const QString& installedJson, QSet<QString> visited, std::function<bool()> current,
    std::function<void(QVariantList)> then)
{
    if (!current()) return;
    // Errors keep the last offers attached, so the user can undo the selection that caused them.
    auto fail = [preview, then](QVariantList rows) {
        if (!rows.isEmpty() && !preview->offers().isEmpty()) {
            QVariantMap first = rows.first().toMap();
            first.insert("optionalDependencies", preview->offers());
            rows[0] = first;
        }
        then(rows);
    };
    if (visited.contains(request) || visited.size() >= 128) {
        fail({QVariantMap{{"name", preview->subject()},
                         {"error", QStringLiteral("dependency selections could not be resolved consistently")}}});
        return;
    }
    visited.insert(request);
    QPointer<PackageCoordinator> self(this);
    LogosModules logos(m_logosAPI);
    logos.package_downloader.resolveDependenciesAsyncResult(request, installedJson,
        [self, preview, request, installedJson, visited, current, then, fail](logos::AsyncResult<QVariantList> result) {
            if (!self || !current()) return;
            if (!result.ok() || result.value.isEmpty()) {
                const QString detail = QString::fromStdString(result.error.message);
                fail({QVariantMap{{"name", preview->subject()}, {"error", detail.isEmpty()
                    ? QStringLiteral("could not resolve package dependencies") : detail}}});
                return;
            }
            // A package no enabled catalog serves may still be installed: resolve
            // again keeping that copy (see logos::installedFallback).
            const QString fallback = logos::installedFallback(
                installedJson, result.value, self->m_installedPackagesCache, preview->subject());
            if (!fallback.isEmpty()) {
                QSet<QString> again = visited;
                again.remove(request);
                self->resolveOptionalPreviewPass(preview, request, fallback, again, current, then);
                return;
            }
            for (const QVariant& v : result.value)
                if (v.toMap().contains("error")) { fail(result.value); return; }
            const QString next = preview->advance(result.value);
            if (next != request) {
                self->resolveOptionalPreviewPass(preview, next, installedJson, visited, current, then);
                return;
            }
            QVariantList resolved;
            bool attached = false;
            for (const QVariant& v : result.value) {
                QVariantMap row = v.toMap();
                row.remove("optionalDependencies");
                row.insert("optional", preview->optionalNames().contains(row.value("name").toString()));
                const QStringList via = preview->requiredFor(row.value("name").toString());
                if (!via.isEmpty()) row.insert("requiredFor", via);
                if (!attached && row.value("name").toString() == preview->subject()) {
                    row.insert("optionalDependencies", preview->offers());
                    attached = true;
                }
                resolved.append(row);
            }
            then(resolved);
        }, Timeout(2 * 60 * 1000));
}

// Optional rows for display: each offer, followed by the packages that come
// only with it (no checkbox; "Required by ..."), so they are not mistaken for
// the subject's own required packages.
QVariantList PackageCoordinator::optionalRowsWithChildren(const QVariantList& offerRows,
                                                          const QVariantList& changes) const
{
    QMap<QString, QString> displayNames;
    for (const QVariant& v : offerRows)
        displayNames.insert(v.toMap().value("name").toString(), v.toMap().value("displayName").toString());
    QVariantList out;
    QSet<QString> placed;
    for (const QVariant& v : offerRows) {
        out.append(v);
        const QString root = v.toMap().value("name").toString();
        for (const QVariant& cv : changes) {
            QVariantMap child = cv.toMap();
            const QStringList via = child.value("requiredFor").toStringList();
            const QString name = child.value("name").toString();
            if (via.isEmpty() || via.first() != root || placed.contains(name)) continue;
            placed.insert(name);
            QStringList by;
            for (const QString& r : via) by.append(displayNames.value(r, r));
            child.remove("versions");   // its version follows the optional that needs it
            child.insert("optional", true);
            child.insert("description", tr("Required by %1").arg(by.join(", ")));
            out.append(child);
        }
    }
    return out;
}

QVariantList PackageCoordinator::gatePreviewChanges(const QVariantList& resolved)
{
    m_pendingOptionalPackages.clear();
    QVariantList changes;
    for (const QVariant& v : resolved)
        for (const QVariant& offer : v.toMap().value("optionalDependencies").toList())
            m_pendingOptionalPackages.append(optionalPackageRow(offer.toMap()));
    const QVariantList all = computeDepChanges(resolved, m_installedVersionByName);
    for (const QVariant& v : all) {
        const QVariantMap row = v.toMap();
        if (row.value("name").toString() == m_pendingPreviewName && !row.contains("error")) continue;
        if (!row.value("optional").toBool() && !row.contains("requiredFor")) changes.append(row);
    }
    changes.append(optionalRowsWithChildren(m_pendingOptionalPackages, all));
    return changes;
}

void PackageCoordinator::refreshOptionalPreview(const QString& name,
                                                 const QVariantMap& optionalSelection,
                                                 const QVariantMap& optionalVersionPins)
{
    if (!m_logosAPI || name.isEmpty()) return;
    if (name != m_pendingPreviewName || (name != m_pendingInstallName
        && !(m_pendingAction.isUpgrade && m_pendingAction.name == name))) return;
    const int epoch = ++m_gatePreviewEpoch;
    emit optionalGatePreviewUpdated(name, {}, true);
    beginTrust(name);
    QPointer<PackageCoordinator> self(this);
    LogosModules logos(m_logosAPI);
    const logos::InstallChoices choices =
        logos::withDialogPicks(m_pendingPreviewChoices, optionalSelection, optionalVersionPins);
    logos.package_manager.getInstalledPackagesAsync(
        [self, name, choices, epoch](QVariantList installed) {
            if (!self || self->m_gatePreviewEpoch != epoch) return;
            self->resolveOptionalPreview(name, self->m_pendingPreviewRepo,
                choices.pins, choices.selection, choices.optionalPins,
                self->m_pendingPreviewNewest ? QString() : logos::installedPackagesJson(installed),
                [self, epoch]() { return self && self->m_gatePreviewEpoch == epoch; },
                [self, name](QVariantList resolved) {
                    if (!self) return;
                    emit self->optionalGatePreviewUpdated(name, self->gatePreviewChanges(resolved), false);
                    self->assessPlanTrust(name, resolved);
                });
        });
}


// ---- Trust at install (utils/InstallTrust.h) -------------------------------

void PackageCoordinator::configureTrust()
{
    const QString keyring = QDir(LogosBasecampPaths::baseDirectory()).filePath(QStringLiteral("keyring"));
    LogosModules modules(m_logosAPI);
    // Only remembered: the folder is made by the first keyring call.
    modules.package_manager.setKeyringDirectory(keyring);
    m_publisherKeyring->configure(keyring);
}

QVariantList PackageCoordinator::trustedPublishers() const
{
    return m_publisherKeyring->publishers();
}

QVariantMap PackageCoordinator::installedRecord(const QString& name) const
{
    return logos::trust::installedRecordOf(m_installedPackagesCache, name);
}

QVariantMap PackageCoordinator::installedTrust(const QString& name) const
{
    const QVariantMap record = installedRecord(name);
    const logos::trust::Package package = logos::trust::fromInstalled(
        name, record.value(QStringLiteral("version")).toString(), record,
        logos::trust::trustedDids(m_publisherKeyring->keys()));
    QVariantMap verdict = trustSummary(name, {package})
                              .value(QStringLiteral("byName")).toMap().value(name).toMap();
    // What the app is, not a row an install plan leaves as it is.
    verdict.remove(QStringLiteral("installed"));
    return verdict;
}

void PackageCoordinator::shellIntentEnded(const QString& dispatchId)
{
    if (dispatchId.isEmpty()) return;
    // As a cancel would, minus the answer: nobody would hear it.
    QString name;
    if (dispatchId == m_pendingInstallRequestId) {
        name = m_pendingInstallName;
        m_pendingInstallRequestId.clear();
        m_pendingInstallName.clear();
    } else if (dispatchId == m_pendingAction.intentRequestId) {
        name = m_pendingAction.op == PendingOp::MultiUninstallCascade ? m_pendingAction.names.value(0)
                                                                      : m_pendingAction.name;
        m_pendingAction = {};
    } else {
        return;
    }
    endGatePreview(name);
    qInfo().noquote() << "PackageCoordinator: the request behind the confirmation for" << name
                      << "ended; closing it";
    emit confirmationWithdrawn(dispatchId);
}

void PackageCoordinator::refreshTrustedPublishers()
{
    withTrustedKeys([]() {});
}

void PackageCoordinator::withTrustedKeys(std::function<void()> then)
{
    if (!m_logosAPI) {
        then();
        return;
    }
    m_publisherKeyring->refresh(std::move(then));
}

void PackageCoordinator::withInstalledAndTrustedKeys(std::function<void()> then)
{
    if (m_dependencyDataReady || !m_logosAPI) {
        withTrustedKeys(std::move(then));
        return;
    }
    auto once = std::make_shared<QMetaObject::Connection>();
    QPointer<PackageCoordinator> self(this);
    *once = connect(this, &PackageCoordinator::dependencyDataReadyChanged, this,
                    [self, once, then]() {
        QObject::disconnect(*once);
        if (self) self->withTrustedKeys(then);
    });
}

void PackageCoordinator::removeTrustedPublisher(const QString& keyName)
{
    m_publisherKeyring->remove(keyName);
}

QStringList PackageCoordinator::installedFromPublisher(const QString& did) const
{
    QStringList names;
    if (did.isEmpty()) return names;
    const QStringList dids = logos::trust::publisherKeys(did);
    for (const QVariant& v : m_installedPackagesCache) {
        const QVariantMap package = v.toMap();
        if (dids.contains(package.value(QStringLiteral("signerDid")).toString())
            && package.value(QStringLiteral("installType")).toString() == QLatin1String("user"))
            names << package.value(QStringLiteral("name")).toString();
    }
    names.removeDuplicates();
    return names;
}

QString PackageCoordinator::gateDispatchId(const QString& name) const
{
    if (name.isEmpty()) return {};
    if (m_pendingInstallName == name && !m_pendingInstallRequestId.isEmpty())
        return m_pendingInstallRequestId;
    if (m_pendingAction.isUpgrade && m_pendingAction.name == name)
        return m_pendingAction.intentRequestId;
    return {};
}

int PackageCoordinator::beginTrust(const QString& name)
{
    m_gateTrust.name = name;
    m_gateTrust.about = gateDispatchId(name);
    m_gateTrust.trust = {{QStringLiteral("pending"), true}};
    m_gateTrust.approval = {};
    ++m_gateTrust.epoch;
    emitTrust();
    return m_gateTrust.epoch;
}

void PackageCoordinator::publishTrust(int epoch, const QVariantMap& trust, const QVariantMap& approval)
{
    if (m_gateTrust.epoch != epoch) return;
    // A gate answered or cancelled is over, and so is what it was told.
    if (gateDispatchId(m_gateTrust.name) != m_gateTrust.about) return;
    m_gateTrust.trust = trust;
    m_gateTrust.approval = approval;
    emitTrust();
}

void PackageCoordinator::emitTrust()
{
    emit installTrustUpdated(m_gateTrust.name, m_gateTrust.trust);
}

void PackageCoordinator::assessPlanTrust(const QString& name, const QVariantList& resolved)
{
    const int epoch = beginTrust(name);
    QPointer<PackageCoordinator> self(this);
    withInstalledAndTrustedKeys([self, name, resolved, epoch]() {
        if (!self) return;
        self->whenCatalogLoaded([self, name, resolved, epoch]() {
            if (!self) return;
            const QList<logos::trust::Package> packages = logos::trust::judgePlan(
                name, resolved,
                [self](const QVariantMap& row) {
                    const QString n = row.value(QStringLiteral("name")).toString();
                    return depAction(self->m_installedVersionByName.value(n),
                                     row.value(QStringLiteral("version")).toString(),
                                     self->m_installedHashByName.value(n),
                                     row.value(QStringLiteral("rootHash")).toString())
                        == QLatin1String("installed");
                },
                [self](const QVariantMap& row) {
                    return self->m_versionsByRepoAndName.value(
                        catalogKey(row.value(QStringLiteral("repositoryUrl")).toString(),
                                   row.value(QStringLiteral("name")).toString()));
                },
                [self](const QVariantMap& row) {
                    return self->installedRecord(row.value(QStringLiteral("name")).toString());
                },
                logos::trust::trustedDids(self->m_publisherKeyring->keys()),
                logos::trust::reservedFor(self->m_bundledNames, self->m_installedPackagesCache, resolved));
            self->publishTrust(epoch, self->trustSummary(name, packages),
                               {{QStringLiteral("plan"),
                                 logos::trust::approvedPlan(name, resolved, packages)}});
        });
    });
}

void PackageCoordinator::beginConfirmationTrust(const QString& name, const QString& version,
                                                bool localFile, const QVariantMap& params,
                                                const QString& requesterName)
{
    // A local .lgx has no catalogue entry: its signer comes from the file,
    // which only a slot allowed to may name (DefaultApps.h), from its bundled
    // app. A catalogue plan, with or without a repository, is assessed once
    // resolved; until then, "checking".
    if (localFile) {
        const auto* defaultApp = logos::defaultapps::appNamed(requesterName);
        const bool fromPackageManager = defaultApp && defaultApp->namesLocalFiles
                                        && requesterIsBundled(defaultApp->app);
        assessLocalFileTrust(name, version, logos::trust::localPackagePath(
            params.value(QStringLiteral("localPath")).toString(), fromPackageManager));
    } else {
        beginTrust(name);
    }
}

QVariantMap PackageCoordinator::trustSummary(const QString& about,
                                             const QList<logos::trust::Package>& packages) const
{
    // A keyring that can't be read says neither who is trusted nor whose name
    // is taken: every signer is one Basecamp couldn't check.
    return logos::trust::summary(m_publisherKeyring->readable() ? packages : logos::trust::unchecked(packages),
                                 m_publisherKeyring->keys(), about);
}

void PackageCoordinator::assessLocalFileTrust(const QString& name, const QString& version,
                                              const QString& path)
{
    const int epoch = beginTrust(name);
    // A file Basecamp may not open, or can't verify, can't be installed:
    // approving it unchecked would leave nothing to hold the install to.
    if (path.isEmpty() || !m_logosAPI) {
        publishTrust(epoch, trustSummary(
            name, {logos::trust::fromVerifiedFile(name, version, {}, {})}));
        return;
    }
    QPointer<PackageCoordinator> self(this);
    withInstalledAndTrustedKeys([self, name, version, path, epoch]() {
        if (!self) return;
        LogosModules modules(self->m_logosAPI);
        modules.package_manager.verifyPackageAsync(path,
            [self, name, version, path, epoch](QVariantMap verified) {
            if (!self) return;
            // Read again for what package_manager_ui is to find before
            // installing it.
            LogosModules modules(self->m_logosAPI);
            modules.package_manager.inspectPackageAsync(path,
                [self, name, version, path, epoch, verified](QVariantMap inspected) {
                if (!self) return;
                // And all of it, byte for byte: neither the root hash nor the
                // signature covers the manifest of an unsigned file. Hashing a
                // large file must not stall the UI, so it runs on the pool;
                // `self` is only read back on its own thread.
                QThreadPool::globalInstance()->start([self, name, version, path, epoch, verified, inspected]() {
                    const QString sha256 = logos::trust::sha256Of(path);
                    QMetaObject::invokeMethod(qApp, [self, name, version, epoch, verified, inspected, sha256]() {
                        if (!self) return;
                        const QVariantMap file =
                            logos::trust::fileIdentity(name, version, verified, inspected, sha256);
                        const logos::trust::Package package = logos::trust::asInstall(
                            file.isEmpty()
                                ? logos::trust::Package{name, version, logos::trust::Status::Invalid}
                                : logos::trust::fromVerifiedFile(
                                      name, version, verified,
                                      logos::trust::trustedDids(self->m_publisherKeyring->keys())),
                            inspected.value(QStringLiteral("type")).toString(),
                            self->installedRecord(name),
                            self->installedRecord(name + QStringLiteral("_backend")),
                            logos::trust::reservedFor(self->m_bundledNames, self->m_installedPackagesCache, {}));
                        self->publishTrust(epoch, self->trustSummary(name, {package}),
                                           {{QStringLiteral("file"), file}});
                    }, Qt::QueuedConnection);
                });
            });
        });
    });
}

void PackageCoordinator::admitTrust(const QString& name, const QString& about,
                                    bool trustPublishers, std::function<void(const QString&)> then)
{
    const TrustVerdict& verdict = m_gateTrust;
    if (about.isEmpty() || verdict.name != name || verdict.about != about
        || verdict.trust.isEmpty() || verdict.trust.value(QStringLiteral("pending")).toBool()) {
        then(QStringLiteral("Basecamp is still checking who signed it."));
        return;
    }
    if (verdict.trust.value(QStringLiteral("blocked")).toBool()) {
        then(QStringLiteral("It includes a package that is damaged or whose signature does not hold."));
        return;
    }
    // Who signed what the dialog is about, and nobody else (summary()'s `trust`).
    const QVariantMap publisher = verdict.trust.value(QStringLiteral("trust")).toMap();
    if (!trustPublishers || publisher.isEmpty()) {
        then({});
        return;
    }
    m_publisherKeyring->trust(publisher, std::move(then));
}
