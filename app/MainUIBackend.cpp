#include "MainUIBackend.h"

#include <QDebug>

#include "IntentBridgeAdapter.h"
#include "IntentBroker.h"
#include "IntentRegistry.h"
#include "ShellIntentChooser.h"
#include "ShellIntentInstaller.h"
#include "ShellIntentEndpoint.h"
#include "UIPluginPresenter.h"
#include "AppsModel.h"
#include "BasecampModelRoles.h"
#include "CoreModuleManager.h"
#include "ModuleInstanceModel.h"
#include "UIPluginManager.h"
#include "PackageCoordinator.h"
#include "BuildInfo.h"

#include <QDebug>
#include <QJSValue>
#include <QTimer>

namespace {
const QStringList kPackageConfirmIntents = {
    QStringLiteral("logos.packages.confirm_install"),
    QStringLiteral("logos.packages.confirm_uninstall"),
    QStringLiteral("logos.packages.confirm_upgrade"),
};

// Of those, the ones no third party has a legitimate reason to raise.
const QStringList kRestrictedToPackageManagerUi = {
    QStringLiteral("logos.packages.confirm_uninstall"),
    QStringLiteral("logos.packages.confirm_upgrade"),
};

} // namespace

MainUIBackend::MainUIBackend(LogosAPI* logosAPI, logos::qt::QtLogosCore* core, QObject* parent)
    : QObject(parent)
    , m_currentActiveSectionIndex(0)
    , m_logosAPI(logosAPI)
    , m_core(core)
    , m_ownsLogosAPI(false)
    , m_coreModuleManager(nullptr)
    , m_uiPluginManager(nullptr)
    , m_packageCoordinator(nullptr)
    , m_uiModulesModel(new ModuleInstanceModel(this))
    , m_coreModulesModel(new ModuleInstanceModel(this))
{
    if (!m_logosAPI) {
        m_logosAPI = new LogosAPI("core", this);
        m_ownsLogosAPI = true;
    }

    // Order matters: CoreModuleManager must exist before UIPluginManager so
    // the latter's ctor can receive a valid pointer; UIPluginManager must
    // exist before PackageCoordinator for the same reason. Qt tears children
    // down in reverse order at destruction, so PackageCoordinator dies first
    // (stops talking to the module), then UIPluginManager (tears down
    // widgets while CoreModuleManager's C API handle is still valid).


    m_appsModel         = new AppsModel(this);

    // ── Intents: constructed FIRST, and the order is load-bearing ───────
    //
    // Qt destroys children in reverse construction order, and IntentBridgeAdapter
    // tracks bridges that UIPluginManager owns indirectly. Built after it, the
    // adapter would die first and those bridge destructors would call
    // bridgeDestroyed() on freed memory. See ~IntentBridgeAdapter.
    m_intentRegistry = new IntentRegistry(this);
    m_intentBroker   = new IntentBroker(m_intentRegistry, /*presenter=*/nullptr, this);
    m_intentAdapter  = new IntentBridgeAdapter(m_intentBroker, this);

    m_coreModuleManager = new CoreModuleManager(m_logosAPI, m_core, this);
    m_uiPluginManager   = new UIPluginManager(m_logosAPI, m_coreModuleManager, this);
    m_packageCoordinator    = new PackageCoordinator(m_logosAPI, m_coreModuleManager, m_uiPluginManager, m_appsModel, this);
    m_appsModel->setInstallRegistry(m_packageCoordinator->installRegistry());

    // Setter-injection closes the cycle — UIPluginManager queries
    // PackageCoordinator for installType / missing-deps when building its
    // uiModules() list, and consumes uiPluginsFetched for its UI-specific
    // metadata cache. See UIPluginManager::setPackageCoordinator for the signal
    // connections it sets up internally.
    m_uiPluginManager->setPackageCoordinator(m_packageCoordinator);

    // Bridges become intent-capable from here on. PluginLoader attaches each
    // ui_qml app's bridge as it loads, before its QML runs.
    m_uiPluginManager->setIntentAdapter(m_intentAdapter);

    m_intentPresenter = new UIPluginPresenter(m_uiPluginManager, this);
    m_intentBroker->setPresenter(m_intentPresenter);
    wireIntents();

    // Forward manager signals into our own signals of the same name. QML
    // binds to these; by funneling through the facade we keep a stable
    // surface regardless of which manager emitted them.
    //
    // UIPluginManager drives uiModulesChanged/launcherAppsChanged on load/
    // unload events; PackageCoordinator's own uiModulesChanged/launcherAppsChanged
    // already flow through UIPluginManager (wired in setPackageCoordinator) so
    // we only need to listen to UIPluginManager here. The refresh slot
    // repopulates m_uiModulesModel in one shot; QML consumers listen to the
    // model's own dataChanged/modelReset signals rather than any signal here.
    connect(m_uiPluginManager, &UIPluginManager::uiModulesChanged,
            this,              &MainUIBackend::refreshUiModulesModel);
    // Clear the Modules Reload overlay once the user-initiated UI refresh
    // lands its first uiModulesChanged (list is already updated by then).
    connect(m_uiPluginManager, &UIPluginManager::uiModulesChanged,
            this, [this]() {
                if (!m_pendingUiModulesRefresh) return;
                m_pendingUiModulesRefresh = false;
                endModulesLoading();
            });
    connect(m_uiPluginManager, &UIPluginManager::launcherAppsChanged,
            this,              &MainUIBackend::launcherAppsChanged);
    connect(m_uiPluginManager, &UIPluginManager::loadingModulesChanged,
            this,              &MainUIBackend::loadingModulesChanged);
    connect(m_uiPluginManager, &UIPluginManager::currentVisibleAppChanged,
            this,              &MainUIBackend::currentVisibleAppChanged);
    connect(m_uiPluginManager, &UIPluginManager::navigateToApps,
            this,              &MainUIBackend::navigateToApps);
    connect(m_uiPluginManager, &UIPluginManager::packageInstallFailedNotice,
            this,              &MainUIBackend::installFailureNoticeRequested);
    connect(m_uiPluginManager, &UIPluginManager::missingDepsPopupRequested,
            this,              &MainUIBackend::missingDepsPopupRequested);
    connect(m_uiPluginManager, &UIPluginManager::unloadCascadeConfirmationRequested,
            this,              &MainUIBackend::unloadCascadeConfirmationRequested);
    connect(m_uiPluginManager, &UIPluginManager::pluginWindowRequested,
            this,              &MainUIBackend::pluginWindowRequested);
    connect(m_uiPluginManager, &UIPluginManager::pluginWindowRemoveRequested,
            this,              &MainUIBackend::pluginWindowRemoveRequested);
    connect(m_uiPluginManager, &UIPluginManager::presentAppRequested,
            this,              &MainUIBackend::presentAppRequested);

    // Forward PackageCoordinator dialog signals. One uninstallPlanRequested
    // serves all four initiators — see buildPlanPayload.
    connect(m_packageCoordinator, &PackageCoordinator::uninstallPlanRequested,
            this,             &MainUIBackend::uninstallPlanRequested);
    connect(m_packageCoordinator, &PackageCoordinator::dependencyDataReadyChanged,
            this,             &MainUIBackend::dependencyDataReadyChanged);
    // Distinct upgrade/downgrade/reinstall cascade signal — same dialog shape
    // as the uninstall variant, but the title + body need the target version +
    // UpgradeMode so a downgrade doesn't look like a bare uninstall.
    connect(m_packageCoordinator, &PackageCoordinator::upgradeCascadeConfirmationRequested,
            this,             &MainUIBackend::upgradeCascadeConfirmationRequested);
    connect(m_packageCoordinator, &PackageCoordinator::installGateConfirmationRequested,
            this,             &MainUIBackend::installGateConfirmationRequested);
    connect(m_packageCoordinator, &PackageCoordinator::requestOpenAddApplicationDialog,
            this,             &MainUIBackend::requestOpenAddApplicationDialog);
    connect(m_packageCoordinator, &PackageCoordinator::addApplicationDataUpdated,
            this,             &MainUIBackend::addApplicationDataUpdated);
    connect(m_packageCoordinator, &PackageCoordinator::launchAppRequested,
            this,             &MainUIBackend::launchAppRequested);
    // The resolver's required packages are cached here and published as a
    // property; QML binds a shell-declared AppsFilterProxy to it. Nothing on
    // this side holds a pointer to that proxy.
    connect(m_packageCoordinator, &PackageCoordinator::requiredPackagesResolved,
            this, [this](const QVariantList& entries) {
        if (m_requiredPackages == entries) return;
        m_requiredPackages = entries;
        emit requiredPackagesChanged();
    });

    // Widened to int at this hop rather than connected signal-to-signal, so
    // the conversion is written down instead of relying on the implicit
    // enum-to-int the connect check would otherwise permit silently.
    connect(m_packageCoordinator, &PackageCoordinator::catalogInstallStageChanged,
            this, [this](const QString& name, InstallStage::Value stage) {
        emit catalogInstallStageChanged(name, static_cast<int>(stage));
    });
    connect(m_packageCoordinator, &PackageCoordinator::catalogInstallFinished,
            this,             &MainUIBackend::catalogInstallFinished);
    connect(m_packageCoordinator, &PackageCoordinator::catalogInstallFailed,
            this,             &MainUIBackend::catalogInstallFailed);

    // Package repositories — pure re-emits so QML binding to backend.*
    connect(m_packageCoordinator, &PackageCoordinator::repositoriesChanged,
            this,             &MainUIBackend::repositoriesChanged);
    connect(m_packageCoordinator, &PackageCoordinator::repositoriesLoadingChanged,
            this,             &MainUIBackend::repositoriesLoadingChanged);
    connect(m_packageCoordinator, &PackageCoordinator::appsLoadingChanged,
            this,             &MainUIBackend::appsLoadingChanged);
    connect(m_packageCoordinator, &PackageCoordinator::repositoryOperationCompleted,
            this,             &MainUIBackend::repositoryOperationCompleted);

    // Any of the three managers can trigger coreModulesChanged:
    //   * CoreModuleManager on stats-tick / refresh
    //   * UIPluginManager on cascade-induced state changes (re-emits
    //     PackageCoordinator's coreModulesChanged as part of that wiring)
    // Both fan into the same refresh slot. Qt coalesces redundant dataChanged
    // notifies within a frame, so the two-connect layout doesn't cause
    // visible flicker.
    connect(m_uiPluginManager,   &UIPluginManager::coreModulesChanged,
            this, &MainUIBackend::refreshCoreModulesModel);
    connect(m_coreModuleManager, &CoreModuleManager::coreModulesChanged,
            this, &MainUIBackend::refreshCoreModulesModel);

    // Kick the first catalog scan now that all wiring is in place. We do
    // this AFTER setPackageCoordinator (and its signal connections) so the
    // resulting uiPluginsFetched / uiModulesChanged land on live slots.
    QTimer::singleShot(0, this, [this]() {
        m_packageCoordinator->refresh();
    });

    qDebug() << "MainUIBackend created";
}

MainUIBackend::~MainUIBackend() = default;

QAbstractItemModel* MainUIBackend::appsModel() const
{
    return m_appsModel;
}

void MainUIBackend::beginShutdown()
{
    if (m_uiPluginManager) {
        m_uiPluginManager->shutdown();
    }
}

int MainUIBackend::currentActiveSectionIndex() const
{
    return m_currentActiveSectionIndex;
}

void MainUIBackend::setCurrentActiveSectionIndex(int index)
{
    // Section list is owned by QML (SidebarPanel). The upper bound is
    // self-policed there; we only guard against negative indices.
    // Per-section side effects (e.g., the Modules-view auto-refresh) live
    // in the QML view that becomes visible, not here.
    if (m_currentActiveSectionIndex != index && index >= 0) {
        m_currentActiveSectionIndex = index;
        emit currentActiveSectionIndexChanged();
    }
}

// --- coreModules() composer ------------------------------------------------
//
// coreModules is the one QML-visible property that spans multiple managers.
// Known + loaded + stats come from CoreModuleManager (raw liblogos state);
// installType comes from PackageCoordinator (populated during its dep-info
// refresh). We compose here so neither manager has to know about the other's
// schema.
QVariantList MainUIBackend::buildCoreModulesSnapshot() const
{
    QVariantList modules;
    if (!m_coreModuleManager) return modules;

    const QStringList known  = m_coreModuleManager->knownModules();
    const QStringList loaded = m_coreModuleManager->loadedModules();

    for (const QString& name : known) {
        QVariantMap module;
        module["name"] = name;
        module["displayName"] = m_packageCoordinator ? m_packageCoordinator->displayNameFor(name) : name;
        module["isLoaded"] = loaded.contains(name);
        // installType populated lazily by refreshDependencyInfo's full-scan
        // pass on PackageCoordinator. Empty means "not known yet" — QML treats
        // that as a non-user module and hides Uninstall, which is the safe
        // default.
        module["installType"] = m_packageCoordinator ? m_packageCoordinator->installType(name) : QString();

        const QVariantMap stats = m_coreModuleManager->moduleStats(name);
        if (!stats.isEmpty()) {
            module["cpu"] = stats["cpu"];
            module["memory"] = stats["memory"];
        } else {
            module["cpu"] = "0.0";
            module["memory"] = "0.0";
        }

        modules.append(module);
    }

    return modules;
}

// --- Manager delegations ---------------------------------------------------
//
// Each slot is a one-liner routing to the right manager. These stay on
// MainUIBackend so the QML `backend.foo(...)` contract is untouched by the
// refactor (QML still sees one receiver).

QVariantList MainUIBackend::buildUiModulesSnapshot() const { return m_uiPluginManager->uiModules(); }

// Refresh slots — fold "compute snapshot + push into model" into one call
// so the ctor can wire signal→slot directly rather than routing through a
// lambda for each source of change.
void MainUIBackend::refreshUiModulesModel()
{
    m_uiModulesModel->replaceRows(buildUiModulesSnapshot());
}

void MainUIBackend::refreshCoreModulesModel()
{
    m_coreModulesModel->replaceRows(buildCoreModulesSnapshot());
}

QVariantList MainUIBackend::launcherApps() const     { return m_uiPluginManager->launcherApps(); }
QString      MainUIBackend::currentVisibleApp() const{ return m_uiPluginManager->currentVisibleApp(); }
QStringList  MainUIBackend::loadingModules() const   { return m_uiPluginManager->loadingModules(); }

// UIPluginManager — UI plugin widget lifecycle + local unload cascade.
void MainUIBackend::loadUiModule(const QString& n)            { m_uiPluginManager->loadUiModule(n); }
void MainUIBackend::unloadUiModule(const QString& n)          { m_uiPluginManager->unloadUiModule(n); }
void MainUIBackend::activateApp(const QString& n)             { m_uiPluginManager->activateApp(n); }
void MainUIBackend::confirmUnloadCascade(const QString& n)    { m_uiPluginManager->confirmUnloadCascade(n); }
void MainUIBackend::loadCoreModule(const QString& n)          { m_uiPluginManager->loadCoreModule(n); }
void MainUIBackend::unloadCoreModule(const QString& n)        { m_uiPluginManager->unloadCoreModule(n); }
void MainUIBackend::refreshUiModules()
{
    if (!m_pendingUiModulesRefresh) {
        beginModulesLoading();
        m_pendingUiModulesRefresh = true;
    }
    m_uiPluginManager->refreshUiModules();
}
void MainUIBackend::onAppLauncherClicked(const QString& n)    { m_uiPluginManager->onAppLauncherClicked(n); }
void MainUIBackend::setCurrentVisibleApp(const QString& n)    { m_uiPluginManager->setCurrentVisibleApp(n); }

// PackageCoordinator — package_manager IPC and package-lifecycle cascade.
void MainUIBackend::wireIntents()
{
    // Registry rebuilds follow the UI-plugin metadata cache, which
    // PackageCoordinator already keeps fresh on every install/uninstall via its
    // event subscriptions (100 ms debounce) and on refresh(). Label and icon
    // are injected as callables so the chooser can never disagree with the
    // sidebar about what an app is called or what it looks like.
    connect(m_uiPluginManager, &UIPluginManager::uiPluginMetadataChanged,
            this, &MainUIBackend::rebuildIntentRegistry);

    // Activation queue drains. appReady is the success edge; the other two are
    // the failure edges. Without the missing-deps one a request sits for the
    // full activation deadline while the user stares at a modal dialog.
    connect(m_uiPluginManager, &UIPluginManager::appReady,
            m_intentBroker, &IntentBroker::onAppReady);
    connect(m_uiPluginManager, &UIPluginManager::pluginLoadFailedNotice,
            this, [this](const QString& name, const QString&) {
                m_intentBroker->onAppUnavailable(name);
            });
    connect(m_uiPluginManager, &UIPluginManager::missingDepsPopupRequested,
            this, [this](const QString& name, const QVariantList&, const QString&) {
                m_intentBroker->onAppUnavailable(name);
            });

    // Chooser signals are re-emitted for QML. The broker itself is NOT put into
    // the QML context — that would break the facade contract this class holds.
    connect(m_intentBroker, &IntentBroker::chooserRequested,
            this,           &MainUIBackend::intentChooserRequested);
    connect(m_intentBroker, &IntentBroker::chooserDismissed,
            this,           &MainUIBackend::intentChooserDismissed);

    // …and the seam that tells the broker whether anyone is actually listening
    // to those signals. Counting receivers on the broker would always report
    // the two connections just made above.
    m_intentChooser = std::make_unique<ShellIntentChooser>(
        [this](const QString& dispatchId, const QString& intent,
               const QString& requesterName, const QVariantList& providers) {
            const int receivers = this->receivers(
                SIGNAL(intentChooserRequested(QString, QString, QString, QVariantList)));
            if (receivers > 0)
                emit intentChooserRequested(dispatchId, intent, requesterName, providers);
            return receivers;
        },
        [this](const QString& dispatchId) {
            emit intentChooserDismissed(dispatchId);
        });
    m_intentBroker->setChooser(m_intentChooser.get());

    // The install suggestion. No receiver count and no withdraw: the request
    // that prompted this is already answered, so a prompt that fails to mount
    // costs nothing and there is nothing to take back.
    m_intentInstaller = std::make_unique<ShellIntentInstaller>(
        [this](const QString& intent, const QStringList& candidates) {
            // Carry WHERE each candidate comes from. A package suggested by a
            // repo the user added for something unrelated deserves to be
            // recognised as such before it is installed.
            QVariantList detailed;
            for (const QString& name : candidates) {
                detailed.append(QVariantMap{
                    {QStringLiteral("moduleName"), name},
                    {QStringLiteral("displayName"), displayNameFor(name)},
                    {QStringLiteral("repositoryUrl"), repositoryUrlFor(name)},
                });
            }
            emit intentInstallOffered(intent, candidates, detailed);
        });
    m_intentBroker->setInstaller(m_intentInstaller.get());

    // Catalog-sourced providers follow the apps model, which is what carries
    // packages that are NOT installed. Kept in a table the resolver never
    // consults — see IntentRegistry::setInstallableProviders.
    if (m_appsModel) {
        // ALL FOUR SIGNALS. Catalog rows arrive via beginInsertRows /
        // endInsertRows, not a model reset — connecting only modelReset and
        // dataChanged meant a freshly fetched catalog never reached the
        // registry, so an intent nothing installed could service went straight
        // to `unavailable` even with a package for it sitting in the catalog.
        connect(m_appsModel, &QAbstractItemModel::modelReset,
                this, &MainUIBackend::rebuildInstallableProviders);
        connect(m_appsModel, &QAbstractItemModel::dataChanged,
                this, &MainUIBackend::rebuildInstallableProviders);
        connect(m_appsModel, &QAbstractItemModel::rowsInserted,
                this, &MainUIBackend::rebuildInstallableProviders);
        connect(m_appsModel, &QAbstractItemModel::rowsRemoved,
                this, &MainUIBackend::rebuildInstallableProviders);
        rebuildInstallableProviders();
    }

    // The shell's endpoint. deliverRequest returns the receiver count of the
    // QML-facing signal: zero means nothing in ContentViews.qml is listening,
    // and the broker must turn that into a timeout rather than waiting forever
    // on a handler that does not exist.
    m_shellEndpoint = std::make_unique<ShellIntentEndpoint>(
        [this](const QString& dispatchId, const QString& intent,
               const QVariantMap& params, const QString& requesterName) {
            // Package confirmations are serviced in C++, NOT through the QML
            // relay below: PackageCoordinator owns the cascade-unload that must
            // finish before the requester is told it may delete anything, the
            // dependent caches the dialog is drawn from, and the pending
            // dispatch id that binds an answer to the dialog it came from.
            //
            // They also could not use the relay's receiver count: that counts
            // ContentViews' connections, and these dialogs live in the separate
            // OverlayDialogs widget.
            if (kPackageConfirmIntents.contains(intent)) {
                return m_packageCoordinator
                    && m_packageCoordinator->beginPackageConfirmation(
                           dispatchId, intent, params, requesterName)
                    ? 1 : 0;
            }
            const int receivers =
                this->receivers(SIGNAL(shellIntentRequested(QString, QString, QVariantMap, QString)));
            if (receivers > 0)
                emit shellIntentRequested(dispatchId, intent, params, requesterName);
            return receivers;
        });
    m_intentBroker->registerEndpoint(QStringLiteral("main_ui"), m_shellEndpoint.get());

    // How PackageCoordinator answers. A callback rather than the broker itself,
    // so it never has to know one exists.
    if (m_packageCoordinator) {
        m_packageCoordinator->setIntentResponder(
            [this](const QString& requestId, bool ok, const QString& error) {
                return respondToShellIntent(requestId, ok, QVariant(), error);
            });
    }

    // The shell's own provided capabilities. main_ui is the only module allowed
    // a "logos.*" name; IntentRegistry refuses that name from any disk record.
    // What the shell itself asks apps for. Declared in code because the shell
    // has no metadata.json, but checked by the broker exactly like an app's.
    m_intentRegistry->registerShellUses(
        QStringLiteral("main_ui"), {QStringLiteral("packages.show")});

    m_intentRegistry->registerShellProvider(
        QStringLiteral("main_ui"),
        QStringList{QStringLiteral("logos.repositories.manage")} + kPackageConfirmIntents,
        QStringLiteral("Logos"),
        QStringLiteral("qrc:/qt/qml/Basecamp/Icons/assets/settings.svg"));

    // confirm_install stays open: an app suggesting "you need X" is legitimate,
    // and the shell already offers catalog installs an app's request provoked.
    // Removal and version changes are restricted — no third party has a use for
    // them, so the prompt would be one whose right answer is always no.
    for (const QString& destructive : kRestrictedToPackageManagerUi) {
        m_intentRegistry->restrictIntentToRequesters(
            destructive, {QStringLiteral("package_manager_ui")});
    }

    rebuildIntentRegistry();
}

void MainUIBackend::rebuildIntentRegistry()
{
    if (!m_intentRegistry || !m_uiPluginManager) return;
    m_intentRegistry->rebuild(
        m_uiPluginManager->uiPluginMetadataSnapshot(),
        [this](const QString& name) { return displayNameFor(name); },
        [this](const QString& name) {
            return m_uiPluginManager->pluginIconUrl(name);
        });

    // Surface what was skipped or refused. Without this a malformed `uses`
    // block is invisible: the app's request comes back `not_declared` and the
    // only clue is on the requester's side, which cannot see why.
    const QStringList problems = m_intentRegistry->diagnostics();
    for (const QString& problem : problems)
        qWarning().noquote() << "IntentRegistry:" << problem;
}

void MainUIBackend::rebuildInstallableProviders()
{
    if (!m_intentRegistry || !m_appsModel) return;

    QMap<QString, QStringList> byModule;
    for (int row = 0; row < m_appsModel->rowCount(); ++row) {
        const QModelIndex idx = m_appsModel->index(row, 0);

        // Installed packages are answered by the real registry. Offering to
        // install something already present would be nonsense.
        if (idx.data(AppsModelRoles::IsInstalledRole).toBool())
            continue;

        const QStringList provides =
            idx.data(AppsModelRoles::ProvidesRole).toStringList();
        if (provides.isEmpty())
            continue;

        byModule.insert(idx.data(AppsModelRoles::NameRole).toString(), provides);
    }
    m_intentRegistry->setInstallableProviders(byModule);

    // One line per rebuild, and only when something is actually offerable —
    // silence here is what an empty catalog looks like, which is exactly the
    // state that made an install offer never appear.
    if (!byModule.isEmpty()) {
        qWarning().noquote() << "IntentRegistry: installable providers ="
                             << QStringList(byModule.keys()).join(QStringLiteral(", "));
    }
}

// ── Intent surface for QML — all delegating one-liners ──────────────────────

bool MainUIBackend::respondToShellIntent(const QString& requestId, bool ok,
                                         const QVariant& data, const QString& error)
{
    // `data` is an untyped QVariant, so QML hands over the QJSValue wrapper
    // untouched — and a QJSValue is engine-bound, hence non-canonical, so the
    // broker would drop the response and mint `failed` at a caller that did
    // nothing wrong. LogosQmlBridge::respond flattens for the same reason on
    // the app path; this is the shell's copy of that boundary.
    const QVariant flat = data.userType() == qMetaTypeId<QJSValue>()
                        ? data.value<QJSValue>().toVariant()
                        : data;

    if (!m_intentBroker || !m_shellEndpoint) return false;
    return m_intentBroker->submitResponse(m_shellEndpoint.get(), requestId, ok, flat, error);
}

void MainUIBackend::resolveIntentChooser(const QString& dispatchId,
                                         const QString& providerName)
{
    if (m_intentBroker) m_intentBroker->resolveChooser(dispatchId, providerName);
}

void MainUIBackend::cancelIntentChooser(const QString& dispatchId)
{
    if (m_intentBroker) m_intentBroker->cancelChooser(dispatchId);
}

QString MainUIBackend::repositoryUrlFor(const QString& packageName) const
{
    if (!m_appsModel) return QString();
    for (int row = 0; row < m_appsModel->rowCount(); ++row) {
        const QModelIndex idx = m_appsModel->index(row, 0);
        if (idx.data(AppsModelRoles::NameRole).toString() == packageName)
            return idx.data(AppsModelRoles::RepositoryUrlRole).toString();
    }
    return QString();   // installed but not in any catalog we know
}

QVariantMap MainUIBackend::providerDetailsFor(const QString& packageName) const
{
    QVariantMap out;
    out.insert(QStringLiteral("moduleName"), packageName);
    out.insert(QStringLiteral("displayName"), displayNameFor(packageName));

    // Every package is unsigned today: the catalog carries no signatures and
    // `trustedSigners` is empty. Stated outright rather than omitted, because a
    // detail panel that lists a version and an origin and says nothing about
    // provenance reads as reassurance it has not earned. When signing lands
    // this becomes a real answer instead of a constant.
    out.insert(QStringLiteral("verified"), false);

    if (m_packageCoordinator) {
        out.insert(QStringLiteral("installType"),
                   m_packageCoordinator->installType(packageName));

        // DELIBERATELY NOT the content hash, though it is available
        // (PackageCoordinator::installedRootHash). A hash is only evidence when
        // the reader holds an independent, trusted value to compare it against,
        // and with nothing signed the catalog that serves the package serves
        // the hash too — so an attacker controlling one controls the other. A
        // tampered copy does not keep the original digest, but it does not need
        // to: it simply presents its own.
        //
        // Shown here it would be worse than absent. A 64-character digest beside
        // the word "Unsigned" reads as rigour, and weakens the one honest
        // statement on the panel. When signing lands it stops being decoration,
        // because a signature is exactly the trusted reference it lacks today.
    }

    if (!m_appsModel)
        return out;

    for (int row = 0; row < m_appsModel->rowCount(); ++row) {
        const QModelIndex idx = m_appsModel->index(row, 0);
        if (idx.data(AppsModelRoles::NameRole).toString() != packageName)
            continue;

        out.insert(QStringLiteral("version"),
                   idx.data(AppsModelRoles::InstalledVersionRole).toString());
        out.insert(QStringLiteral("repositoryUrl"),
                   idx.data(AppsModelRoles::RepositoryUrlRole).toString());
        out.insert(QStringLiteral("description"),
                   idx.data(AppsModelRoles::DescriptionRole).toString());
        break;
    }
    return out;
}

void MainUIBackend::showPackageDetails(const QString& packageName)
{
    if (packageName.isEmpty()) return;

    // Ask whoever provides `packages.show` — in practice the Package Manager,
    // which renders it in its own details panel.
    //
    // An intent rather than a direct call into package_manager_ui on purpose.
    // basecamp used to carry exactly one hardcoded edge into that app — a .rep
    // SIGNAL, a replica connect, two forwarding C++ signals and a QML handler,
    // all to reach one settings screen — and it has since been deleted in
    // favour of the generic path. Adding a second would be walking backwards.
    // This also means the shell dogfoods the mechanism it asks apps to use.
    if (m_intentBroker && m_shellEndpoint
        && m_registryDeclaresPackagesShow()) {
        m_intentBroker->submit(m_shellEndpoint.get(),
                               QStringLiteral("shell-details-") + packageName,
                               QStringLiteral("packages.show"),
                               QVariantMap{{QStringLiteral("name"), packageName}});
        return;
    }

    showPackageDetailsFallback(packageName);
}

bool MainUIBackend::m_registryDeclaresPackagesShow() const
{
    return m_intentRegistry
        && m_intentRegistry->resolve(QStringLiteral("packages.show")).status
               != IntentRegistry::None;
}

void MainUIBackend::showPackageDetailsFallback(const QString& packageName)
{
    if (!m_packageCoordinator) return;

    // Nothing provides packages.show — no Package Manager installed, or an
    // older build of it. The App Manager's own detail dialog answers the same
    // question (version, publisher, dependencies, install state), so the
    // Details button still does something rather than silently failing.
    //
    // allowFastLaunch=false is the point: the user asked to LOOK, not to run.
    m_packageCoordinator->openApp(packageName, repositoryUrlFor(packageName),
                                  {}, /*allowFastLaunch=*/false);
}

void MainUIBackend::beginIntentInstall(const QString& providerName)
{
    if (!m_packageCoordinator || providerName.isEmpty())
        return;

    // The normal install path, gate dialog and all. Being prompted by an intent
    // buys no shortcut past the confirmation the user would see anywhere else —
    // it is the same install, started for a different reason.
    m_packageCoordinator->openApp(providerName, repositoryUrlFor(providerName), {},
                                  /*allowFastLaunch=*/false);
}

QString MainUIBackend::displayNameFor(const QString& n) const {
    return m_packageCoordinator ? m_packageCoordinator->displayNameFor(n) : n;
}
void MainUIBackend::uninstallUiModule(const QString& n)       { m_packageCoordinator->uninstallUiModule(n); }
void MainUIBackend::uninstallApp(const QString& n, const QString& repositoryUrl)
                                                             { m_packageCoordinator->uninstallApp(n, repositoryUrl); }
void MainUIBackend::uninstallCoreModule(const QString& n)     { m_packageCoordinator->uninstallCoreModule(n); }
void MainUIBackend::confirmUninstallCascade(const QString& n) { m_packageCoordinator->confirmUninstallCascade(n); }
void MainUIBackend::confirmUninstallMultiCascade(const QStringList& names) { m_packageCoordinator->confirmUninstallMultiCascade(names); }
void MainUIBackend::cancelMultiUninstall(const QStringList& names)         { m_packageCoordinator->cancelMultiUninstall(names); }
void MainUIBackend::cancelPendingUninstallApp(const QString& name)         { m_packageCoordinator->cancelPendingUninstallApp(name); }
void MainUIBackend::confirmInstallGate(const QString& n)      { m_packageCoordinator->confirmInstallGate(n); }
void MainUIBackend::cancelInstallGate(const QString& n)       { m_packageCoordinator->cancelInstallGate(n); }
void MainUIBackend::openApp(const QString& name, const QString& repositoryUrl, const QVariantMap& versionPins, bool allowFastLaunch)
{ m_packageCoordinator->openApp(name, repositoryUrl, versionPins, allowFastLaunch); }
void MainUIBackend::confirmCatalogInstall(const QString& name, const QString& repositoryUrl, const QVariantMap& versionPins)
{ m_packageCoordinator->confirmCatalogInstall(name, repositoryUrl, versionPins); }
void MainUIBackend::notifyAddApplicationDialogClosed()
{ m_packageCoordinator->notifyAddApplicationDialogClosed(); }

// cancelPendingAction is the one slot that doesn't route to a single manager:
// a pending action lives on either UIPluginManager (local unload cascade) or
// PackageCoordinator (uninstall/upgrade cascade) but not both. Fan out to both —
// the un-involved manager no-ops on name-mismatch. This preserves the QML
// contract (single `backend.cancelPendingAction(name)` call for either dialog).
void MainUIBackend::cancelPendingAction(const QString& n) {
    m_uiPluginManager->cancelUnloadCascade(n);
    m_packageCoordinator->cancelPendingAction(n);
}

// Package repositories — delegations + cache pass-through.
QVariantList MainUIBackend::repositories() const        { return m_packageCoordinator->repositories(); }
bool         MainUIBackend::repositoriesLoading() const { return m_packageCoordinator->repositoriesLoading(); }
bool MainUIBackend::appsLoading() const
{ return !m_packageCoordinator || m_packageCoordinator->appsLoading(); }

bool MainUIBackend::dependencyDataReady() const
{ return m_packageCoordinator && m_packageCoordinator->dependencyDataReady(); }

bool MainUIBackend::modulesLoading() const
{ return m_modulesLoadingCount > 0; }

void MainUIBackend::beginModulesLoading()
{
    if (++m_modulesLoadingCount == 1)
        emit modulesLoadingChanged();
}

void MainUIBackend::endModulesLoading()
{
    if (m_modulesLoadingCount <= 0) return;
    if (--m_modulesLoadingCount == 0)
        emit modulesLoadingChanged();
}

void MainUIBackend::refreshRepositories()                                  { m_packageCoordinator->refreshRepositories(); }
void MainUIBackend::refreshAppCatalog()                                    { m_packageCoordinator->remoteRefresh(); }
void MainUIBackend::addRepository(const QString& url)                      { m_packageCoordinator->addRepository(url); }
void MainUIBackend::removeRepository(const QString& url)                   { m_packageCoordinator->removeRepository(url); }
void MainUIBackend::setRepositoryEnabled(const QString& url, bool enabled) { m_packageCoordinator->setRepositoryEnabled(url, enabled); }

// --- CoreModuleManager delegations ----------------------------------------

void MainUIBackend::refreshCoreModules()
{
    beginModulesLoading();
    m_coreModuleManager->refresh();
    QTimer::singleShot(0, this, [this]() { endModulesLoading(); });
}
QString MainUIBackend::getCoreModuleMethods(const QString& n)       { return m_coreModuleManager->getMethods(n); }
QString MainUIBackend::getCoreModuleEvents(const QString& n)        { return m_coreModuleManager->getEvents(n); }
QString MainUIBackend::callCoreModuleMethod(const QString& n,
                                             const QString& m,
                                             const QString& a)      { return m_coreModuleManager->callMethod(n, m, a); }

// --- Build info -----------------------------------------------------------
//
// Thin QML-facing wrappers over the shared LogosBasecampBuildInfo helper
// (app/utils/BuildInfo.h), which reads the nix-generated logos_build_info.h.

QString      MainUIBackend::buildVersion() const    { return LogosBasecampBuildInfo::version(); }
bool         MainUIBackend::isPortableBuild() const { return LogosBasecampBuildInfo::isPortableBuild(); }
QVariantList MainUIBackend::buildCommits() const    { return LogosBasecampBuildInfo::commits(); }

