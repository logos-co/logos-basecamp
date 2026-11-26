#include "MainUIBackend.h"
#include "logos_api_client.h"

#include <QDebug>

#include "IntentBridgeAdapter.h"
#include "IntentBroker.h"
#include "IntentRegistry.h"
#include "ShellIntentChooser.h"
#include "ShellIntentInstaller.h"
#include "ShellIntentEndpoint.h"
#include "ShellIntents.h"
#include "DefaultApps.h"
#include "IntentChooserQueue.h"
#include "ConfirmationQueue.h"
#include "links/LinkRequestCoordinator.h"
#include "UIPluginPresenter.h"
#include "intents/IntentsModuleHost.h"
#include "AppsModel.h"
#include "BasecampModelRoles.h"
#include "ShellSections.h"
#include "CoreModuleManager.h"
#include "ModuleInstanceModel.h"
#include "UIPluginManager.h"
#include "PackageCoordinator.h"
#include "BuildInfo.h"
#include "utils/BuiltInApps.h"
#include "utils/InstalledPlacement.h"
#include "utils/LogosBasecampPaths.h"
#include "utils/RevealInFileManager.h"

#include <QDebug>
#include <QJSValue>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QLoggingCategory>
#include <QMap>
#include <QNetworkInformation>
#include <QTimer>

#include <tuple>
#include <type_traits>

// Off below warnings by default:
//   QT_LOGGING_RULES="logos.basecamp.intents.debug=true"
Q_LOGGING_CATEGORY(lcBasecampIntents, "logos.basecamp.intents", QtInfoMsg)

// The shell's own capability tables and their registration live in
// ShellIntents so a unit test can reach them; this file cannot be linked into
// the test harness. Unqualified below to keep the use sites unchanged.
using namespace ShellIntents;

namespace {

template <typename> struct SignalArgs;
template <typename Class, typename... Args>
struct SignalArgs<void (Class::*)(Args...)> { using type = std::tuple<std::decay_t<Args>...>; };

// Re-emits `signal` as the backend's `reemitted`. connect() accepts a re-emit
// that declares fewer arguments and drops the rest without a word, so it must
// declare every one.
template <typename Sender, typename Signal, typename Reemitted>
void reemit(Sender* sender, Signal signal, MainUIBackend* backend, Reemitted reemitted)
{
    static_assert(std::is_same_v<typename SignalArgs<Signal>::type,
                                 typename SignalArgs<Reemitted>::type>,
                  "a re-emitted signal must declare every argument of the one it re-emits");
    QObject::connect(sender, signal, backend, reemitted);
}

} // namespace

MainUIBackend::MainUIBackend(LogosAPI* logosAPI, ICoreRuntime* core,
                             const LogosBasecamp::HostProfile& hostProfile,
                             IntentsModuleHost* intentsHost, QObject* parent)
    : QObject(parent)
    , m_hostProfile(hostProfile)
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
    m_chooserQueue   = new IntentChooserQueue(this);
    m_confirmations  = new ConfirmationQueue(this);
    connect(m_chooserQueue, &IntentChooserQueue::show, this,
            [this](const QString& id, const QString& intent, const QString& requester,
                   const QVariantList& providers) {
                // Its details and its not-installed section come from the
                // catalog, which is fetched the first time something needs it.
                if (m_packageCoordinator) m_packageCoordinator->requestCatalog();
                emit intentChooserRequested(id, intent, requester, providers);
            });
    connect(m_chooserQueue, &IntentChooserQueue::close, this, &MainUIBackend::intentChooserDismissed);
    // Claimed in main.cpp as soon as intents_module loaded; null when it is off.
    m_intentsHost = m_hostProfile.intentsModule ? intentsHost : nullptr;
    if (m_intentsHost) {
        m_intentsHost->setParent(this);
        m_intentsHost->setAttention(m_hostProfile.intentsAttention);
    } else if (intentsHost) {
        delete intentsHost;
    }

    m_coreModuleManager = new CoreModuleManager(m_logosAPI, m_core, this);
    // Only what the package manager installed is integrity-checked: the user
    // modules dir, which standalone mode does not use.
    if (m_hostProfile.useUserModules)
        m_coreModuleManager->setInstalledModulesRoot(LogosBasecampPaths::modulesDirectory());
    m_uiPluginManager   = new UIPluginManager(m_logosAPI, m_coreModuleManager, m_hostProfile, this);
    m_packageCoordinator    = new PackageCoordinator(m_logosAPI, m_coreModuleManager, m_uiPluginManager, m_appsModel, m_hostProfile, this);
    // A confirmation it can't show at all is answered here: nobody else will.
    m_confirmations->setBegin([this](const ConfirmationQueue::Request& r) {
        if (m_packageCoordinator->beginPackageConfirmation(r.id, r.intent, r.params, r.requester))
            return true;
        respondToShellIntent(r.id, false, QVariant(), QStringLiteral("unavailable"));
        return false;
    });
    connect(m_packageCoordinator, &PackageCoordinator::installedPackagesRead,
            m_confirmations, &ConfirmationQueue::installedChanged);
    connect(m_packageCoordinator, &PackageCoordinator::packageOnItsWay,
            m_confirmations, &ConfirmationQueue::progressed);
    connect(m_packageCoordinator, &PackageCoordinator::packageFilesChanged,
            m_confirmations, &ConfirmationQueue::activity);
    connect(m_uiPluginManager, &UIPluginManager::installerChangeEnded,
            m_confirmations, &ConfirmationQueue::changeEnded);
    // The app making a change closed: nothing is making it any more.
    connect(m_uiPluginManager, &UIPluginManager::uiModulesChanged, this, [this]() {
        const QString changing = m_confirmations->changing();
        if (!changing.isEmpty() && !m_uiPluginManager->isUiAppLoaded(changing))
            m_confirmations->changeEnded(changing);
    });
    // The platform's own backend: by features alone Qt can pick glib, which
    // outside Linux never sees the connection go.
    if (QNetworkInformation::loadDefaultBackend()) {
        qInfo() << "MainUIBackend: reachability from"
                << QNetworkInformation::instance()->backendName();
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged,
                this, &MainUIBackend::onlineChanged);
    }

    // Setter-injection closes the cycle — UIPluginManager queries
    // PackageCoordinator for installType / missing-deps when building its
    // uiModules() list, and consumes uiPluginsFetched for its UI-specific
    // metadata cache. See UIPluginManager::setPackageCoordinator for the signal
    // connections it sets up internally.
    m_uiPluginManager->setPackageCoordinator(m_packageCoordinator);

    // Bridges become intent-capable from here on. PluginLoader attaches each
    // ui_qml app's bridge as it loads, before its QML runs.
    m_uiPluginManager->setIntentAdapter(m_intentAdapter);

    m_intentPresenter = new UIPluginPresenter(
        m_uiPluginManager,
        [this]() { return m_currentActiveSectionIndex; },
        this);
    m_intentPresenter->setDialogProbe([this]() { return m_overlayActive; });
    m_intentBroker->setPresenter(m_intentPresenter);
    wireIntents();
    if (m_intentsHost) wireIntentsModule();

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
    reemit(m_uiPluginManager, &UIPluginManager::launcherAppsChanged,
           this,              &MainUIBackend::launcherAppsChanged);
    reemit(m_uiPluginManager, &UIPluginManager::recentlyClosedAppsChanged,
           this,              &MainUIBackend::recentlyClosedAppsChanged);
    reemit(m_uiPluginManager, &UIPluginManager::loadingModulesChanged,
           this,              &MainUIBackend::loadingModulesChanged);
    reemit(m_uiPluginManager, &UIPluginManager::stoppedAppsChanged,
           this,              &MainUIBackend::stoppedAppsChanged);
    reemit(m_uiPluginManager, &UIPluginManager::currentVisibleAppChanged,
           this,              &MainUIBackend::currentVisibleAppChanged);
    reemit(m_uiPluginManager, &UIPluginManager::navigateToApps,
           this,              &MainUIBackend::navigateToApps);
    reemit(m_uiPluginManager, &UIPluginManager::packageInstallFailedNotice,
           this,              &MainUIBackend::installFailureNoticeRequested);
    reemit(m_uiPluginManager, &UIPluginManager::pluginLoadFailedNotice,
           this,              &MainUIBackend::loadFailureNoticeRequested);
    reemit(m_uiPluginManager, &UIPluginManager::missingDepsPopupRequested,
           this,              &MainUIBackend::missingDepsPopupRequested);
    reemit(m_uiPluginManager, &UIPluginManager::unloadCascadeConfirmationRequested,
           this,              &MainUIBackend::unloadCascadeConfirmationRequested);
    reemit(m_uiPluginManager, &UIPluginManager::pluginWindowRequested,
           this,              &MainUIBackend::pluginWindowRequested);
    reemit(m_uiPluginManager, &UIPluginManager::pluginWindowRemoveRequested,
           this,              &MainUIBackend::pluginWindowRemoveRequested);
    reemit(m_uiPluginManager, &UIPluginManager::presentAppRequested,
           this,              &MainUIBackend::presentAppRequested);

    // Forward PackageCoordinator dialog signals. One uninstallPlanRequested
    // serves one package or several — see buildPlanPayload.
    reemit(m_packageCoordinator, &PackageCoordinator::uninstallPlanRequested,
           this,             &MainUIBackend::uninstallPlanRequested);
    reemit(m_packageCoordinator, &PackageCoordinator::dependencyDataReadyChanged,
           this,             &MainUIBackend::dependencyDataReadyChanged);
    // Distinct upgrade/downgrade/reinstall cascade signal — same dialog shape
    // as the uninstall variant, but the title + body need the target version +
    // UpgradeMode so a downgrade doesn't look like a bare uninstall.
    reemit(m_packageCoordinator, &PackageCoordinator::upgradeCascadeConfirmationRequested,
           this,             &MainUIBackend::upgradeCascadeConfirmationRequested);
    reemit(m_packageCoordinator, &PackageCoordinator::optionalGatePreviewUpdated,
           this, &MainUIBackend::optionalGatePreviewUpdated);
    reemit(m_packageCoordinator, &PackageCoordinator::installGateConfirmationRequested,
           this,             &MainUIBackend::installGateConfirmationRequested);
    reemit(m_packageCoordinator, &PackageCoordinator::confirmationWithdrawn,
           this,             &MainUIBackend::confirmationWithdrawn);
    connect(m_packageCoordinator, &PackageCoordinator::launchAppRequested,
            this, [this](const QString& name) {
        m_uiPluginManager->onAppLauncherClicked(name);
    }, Qt::QueuedConnection);
    connect(m_packageCoordinator, &PackageCoordinator::appManagerRequested,
            this, &MainUIBackend::requestPackageInstall, Qt::QueuedConnection);

    // Package repositories — pure re-emits so QML binding to backend.*
    reemit(m_packageCoordinator, &PackageCoordinator::repositoriesChanged,
           this,             &MainUIBackend::repositoriesChanged);
    reemit(m_packageCoordinator, &PackageCoordinator::installTrustUpdated,
           this,             &MainUIBackend::installTrustUpdated);
    reemit(m_packageCoordinator, &PackageCoordinator::trustedPublishersChanged,
           this,             &MainUIBackend::trustedPublishersChanged);
    reemit(m_packageCoordinator, &PackageCoordinator::trustedPublisherChangeFailed,
           this,             &MainUIBackend::trustedPublisherChangeFailed);
    reemit(m_packageCoordinator, &PackageCoordinator::installRefused,
           this,             &MainUIBackend::installFailureNoticeRequested);
    connect(m_packageCoordinator, &PackageCoordinator::builtInCopySetAside,
            this,             &MainUIBackend::noticeBuiltInCopy);
    connect(m_coreModuleManager,  &CoreModuleManager::builtInCopySetAside,
            this,             &MainUIBackend::noticeBuiltInCopy);
    reemit(m_packageCoordinator, &PackageCoordinator::repositoriesLoadingChanged,
           this,             &MainUIBackend::repositoriesLoadingChanged);
    reemit(m_packageCoordinator, &PackageCoordinator::downloadSourceChanged,
           this,             &MainUIBackend::downloadSourceChanged);
    reemit(m_packageCoordinator, &PackageCoordinator::onboardingRequiredChanged,
           this,             &MainUIBackend::onboardingRequiredChanged);
    reemit(m_packageCoordinator, &PackageCoordinator::storageStateChanged,
           this,             &MainUIBackend::storageStateChanged);
    reemit(m_packageCoordinator, &PackageCoordinator::appsLoadingChanged,
           this,             &MainUIBackend::appsLoadingChanged);
    reemit(m_packageCoordinator, &PackageCoordinator::repositoryOperationCompleted,
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

    // What main() found before this window existed. After the event loop
    // starts, so the QML is up to show it; several share one dialog.
    const auto notices = logos::integrity::takeStartupNotices();
    if (!notices.isEmpty()) {
        QTimer::singleShot(0, this, [this, notices]() {
            if (notices.size() == 1) {
                emit startupNoticeRequested(notices.first().title, notices.first().module,
                                            notices.first().message);
                return;
            }
            QStringList names, messages;
            for (const auto& notice : notices) {
                names << notice.module;
                messages << notice.module + QStringLiteral(": ") + notice.message;
            }
            emit startupNoticeRequested(notices.first().title, names.join(QStringLiteral(", ")),
                                        messages.join(QStringLiteral("\n\n")));
        });
    }

    qDebug() << "MainUIBackend created";
}

MainUIBackend::~MainUIBackend() = default;

QAbstractItemModel* MainUIBackend::appsModel() const
{
    return m_appsModel;
}

void MainUIBackend::beginShutdown()
{
    if (m_intentsHost) m_intentsHost->shutdown();   // modules stopping now stay stopped
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
        if (m_uiPluginManager && m_uiPluginManager->isAppBackend(name)) continue;
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
QVariantList MainUIBackend::recentlyClosedApps() const { return m_uiPluginManager->recentlyClosedApps(); }
QString      MainUIBackend::currentVisibleApp() const{ return m_uiPluginManager->currentVisibleApp(); }
QStringList  MainUIBackend::loadingModules() const   { return m_uiPluginManager->loadingModules(); }
QStringList  MainUIBackend::stoppedApps() const      { return m_uiPluginManager->stoppedApps(); }

// UIPluginManager — UI plugin widget lifecycle + local unload cascade.
void MainUIBackend::loadUiModule(const QString& n)            { m_uiPluginManager->loadUiModule(n); }
void MainUIBackend::unloadUiModule(const QString& n)          { m_uiPluginManager->unloadUiModule(n); }
void MainUIBackend::restartUiModule(const QString& n)         { m_uiPluginManager->restartUiModule(n); }
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
void MainUIBackend::setOverlayActive(bool active)             { m_overlayActive = active; }

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
    connect(m_uiPluginManager, &UIPluginManager::appBackendStopped,
            this, [this](const QString& name, QWidget* widget) {
                m_intentBroker->onAppUnavailable(name);
                m_confirmations->changeEnded(name);
                emit appStopped(widget, name);
            });
    connect(m_uiPluginManager, &UIPluginManager::missingDepsPopupRequested,
            this, [this](const QString& name, const QVariantList&, const QString&) {
                m_intentBroker->onAppUnavailable(name);
            });

    // The chooser reaches QML through this seam alone, which also tells the
    // broker whether anyone is listening. The broker itself is NOT put into
    // the QML context — that would break the facade contract this class holds.
    m_intentChooser = std::make_unique<ShellIntentChooser>(
        [this](const QString& dispatchId, const QString& intent,
               const QString& requesterName, const QVariantList& providers) {
            const int receivers = this->receivers(
                SIGNAL(intentChooserRequested(QString, QString, QString, QVariantList)));
            if (receivers > 0)
                m_chooserQueue->request({dispatchId, intent, requesterName, providers});
            return receivers;
        },
        [this](const QString& dispatchId) {
            m_chooserQueue->withdraw(dispatchId);
        });
    m_intentBroker->setChooser(m_intentChooser.get());

    // The install suggestion. No receiver count and no withdraw: the request
    // that prompted this is already answered, so a prompt that fails to mount
    // costs nothing and there is nothing to take back.
    m_intentInstaller = std::make_unique<ShellIntentInstaller>(
        [this](const QString& intent, const QStringList& candidates) {
            QVariantList detailed;
            for (const QString& name : candidates)
                detailed.append(installCandidateDetails(name));
            emit intentInstallOffered(intent, candidates, detailed);
        },
        // The catalog is fetched the first time something needs it, so "no
        // candidate" may only mean "not fetched yet": look again once it is.
        [this](const QString& intent) {
            if (!m_packageCoordinator || m_packageCoordinator->catalogFetched())
                return;
            m_packageCoordinator->whenCatalogLoaded([this, intent]() {
                const QStringList candidates = m_intentRegistry
                    ? m_intentRegistry->installableProvidersFor(intent)
                    : QStringList{};
                if (!candidates.isEmpty())
                    m_intentInstaller->offerInstall(intent, candidates);
            });
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
            return deliverShellIntent(dispatchId, intent, params, requesterName);
        });
    // The confirmation lets go of it before the queue shows the next, which
    // it would otherwise refuse as one already open.
    m_shellEndpoint->setOnEnded([this](const QString& dispatchId) {
        if (m_packageCoordinator) m_packageCoordinator->shellIntentEnded(dispatchId);
        m_confirmations->withdrawn(dispatchId);
    });
    m_intentBroker->registerEndpoint(QStringLiteral("main_ui"), m_shellEndpoint.get());

    // How PackageCoordinator answers. A callback rather than the broker itself,
    // so it never has to know one exists.
    if (m_packageCoordinator) {
        m_packageCoordinator->setIntentResponder(
            [this](const QString& requestId, bool ok, const QString& error, const QVariant& data,
                   std::function<void(bool)> heard) {
                answerShellIntent(requestId, ok, data, error,
                                  [this, requestId, ok, data, heard = std::move(heard)](bool taken) {
                                      m_confirmations->answered(requestId, ok && taken, data);
                                      if (heard) heard(taken);
                                  });
            });
    }

    // The shell's own provided capabilities, its `uses`, and the requester
    // restrictions on the destructive ones. main_ui is the only module allowed
    // a "basecamp.*" name; IntentRegistry refuses that namespace, and the
    // platform's "logos.*", from any disk record.
    //
    // In ShellIntents rather than inline so shell_intents_test.cpp can assert
    // against this exact registration — this file cannot be linked into the
    // unit-test harness, and the shell's capability list is the last thing that
    // should be covered only by reading.
    ShellIntents::registerWith(
        m_intentRegistry,
        QStringLiteral("main_ui"),
        QStringLiteral("Logos"),
        QStringLiteral("qrc:/qt/qml/Basecamp/Icons/assets/settings.svg"));

    // The synthetic requester a clicked URL submits under. Registered as a
    // REQUESTER ONLY — never through registerShellProvider — because the broker
    // skips the chooser when the shell is either party, and a URL inheriting
    // that would dispatch with no consent at all.
    //
    // Its `uses` is derived on every rebuild from these plus every app that
    // opted in with `"web": true`, so the broker's ordinary `declaresUse` gate
    // is what enforces web reachability. No link-shaped special case exists
    // anywhere inside the broker.
    m_intentRegistry->registerLinkRequester(
        LinkRequestCoordinator::requesterName(), ShellIntents::kWebReachableIntents);

    m_linkRequests = new LinkRequestCoordinator(m_intentBroker, m_intentRegistry, this);
    reemit(m_linkRequests, &LinkRequestCoordinator::linkFailed,
           this, &MainUIBackend::linkFailed);
    if (m_hostProfile.packageCatalog && m_packageCoordinator) {
        m_linkRequests->setCatalogGate([this](std::function<void()> then) {
            m_packageCoordinator->whenCatalogLoaded(std::move(then));
        });
    }
    // With intents_module, links go through it alone, as the link requester.
    if (m_intentsHost) {
        m_linkRequests->setSubmitter([this](const QString& requestId, const QString& intent,
                                            const QVariantMap& params) {
            QPointer<LinkRequestCoordinator> links(m_linkRequests);
            // A refusal comes back as a `failed` result, like any other.
            m_intentsHost->shellRequest(intent, params, /*asLink=*/true,
                [this, links, requestId, intent](const QVariantMap& envelope) {
                    // Nothing installed publishes it to the web; a package not installed yet may.
                    if (envelope.value(QStringLiteral("error")).toString() == QStringLiteral("not_declared")
                        && m_intentRegistry) {
                        const QStringList candidates = m_intentRegistry->installableFor(
                            LinkRequestCoordinator::requesterName(), intent);
                        if (!candidates.isEmpty()) m_intentInstaller->offerInstall(intent, candidates);
                    }
                    if (links) links->resultArrived(requestId, envelope);
                });
            return true;
        });
    }

    rebuildIntentRegistry();
    m_intentRegistryBootstrapped = true;
}

int MainUIBackend::deliverShellIntent(const QString& dispatchId, const QString& intent,
                                      const QVariantMap& params, const QString& requesterName)
{
    // Package confirmations are serviced in C++, NOT through the QML
    // relay below: PackageCoordinator owns the cascade-unload that must
    // finish before the requester is told it may delete anything, the
    // dependent caches the dialog is drawn from, and the pending
    // dispatch id that binds an answer to the dialog it came from.
    //
    // They also could not use the relay's receiver count: that counts
    // ContentViews' connections, and these dialogs live in the separate
    // OverlayDialogs widget.
    // Also serviced in C++ rather than the QML relay: it takes a
    // parameter, has to decide whether the named app is installed, and
    // must answer on a timing floor. A `case` in ContentViews' switch
    // could do none of that.
    if (intent == kAppLaunchIntent)
        return beginAppLaunch(dispatchId, params);

    // One approved change at a time, whichever app asks (ConfirmationQueue).
    if (kPackageConfirmIntents.contains(intent)) {
        if (!m_packageCoordinator) return 0;
        m_confirmations->submit({dispatchId, intent, params, requesterName});
        return 1;
    }
    const int receivers =
        this->receivers(SIGNAL(shellIntentRequested(QString, QString, QVariantMap, QString)));
    if (receivers > 0)
        emit shellIntentRequested(dispatchId, intent, params, requesterName);
    return receivers;
}

void MainUIBackend::wireIntentsModule()
{
    if (!m_intentsHost->isClaimed()) return;
#ifdef ENABLE_QML_INSPECTOR
    m_intentsHost->setAcceptSynthesizedInput(true);
#endif
    m_intentsHost->declareShell(ShellIntents::moduleDeclaration(
        QStringLiteral("main_ui"),
        QStringLiteral("Logos"),
        QStringLiteral("qrc:/qt/qml/Basecamp/Icons/assets/settings.svg"),
        LinkRequestCoordinator::requesterName()));
    m_intentsHost->setPresenter(m_intentPresenter);
    m_intentsHost->setShellProvider([this](const QString& id, const QString& intent,
                                           const QVariantMap& params, const QString& requester) {
        return deliverShellIntent(id, intent, params, requester);
    });
    // A core module the person chose starts as the Module Inspector would
    // start it; an app's backend starts with its app, in the host.
    m_intentsHost->setAppOfModule([this](const QString& module) {
        return m_uiPluginManager->appOfBackend(module);
    });
    // Its package: an app's backend ships in its app's.
    m_intentsHost->setRequesterBundled([this](const QString& module) {
        const QString app = m_uiPluginManager->appOfBackend(module);
        return m_packageCoordinator
            && m_packageCoordinator->requesterIsBundled(app.isEmpty() ? module : app);
    });
    // Basecamp's own requests go to the default app for them with no chooser:
    // its Install to App Manager, its Details to Package Manager.
    m_intentsHost->setDefaultProvider([](const QString& intent) {
        return logos::defaultapps::defaultProviderFor(intent);
    });
    m_intentsHost->setProviderStarter([this](const QString& provider, std::function<void()> done) {
        if (!m_coreModuleManager->knownModules().contains(provider)
            || m_coreModuleManager->loadedModules().contains(provider)) {
            done();
            return;
        }
        m_coreModuleManager->loadModuleInBackground(provider, [done](bool) { done(); });
    });
    m_uiPluginManager->setIntentsModuleHost(m_intentsHost);

    // Pulled from the event loop, never from inside the event's callback.
    if (LogosAPIClient* client = m_logosAPI->getClient(QStringLiteral("intents_module"))) {
        client->onEventWhenAvailable(
            QStringLiteral("intents_module"), QStringLiteral("work_available"),
            [host = QPointer<IntentsModuleHost>(m_intentsHost)](const QString&, const QVariantList&) {
                if (host) QMetaObject::invokeMethod(host, &IntentsModuleHost::pullWork, Qt::QueuedConnection);
            });
    }
    // What was asked before anyone listened.
    QMetaObject::invokeMethod(m_intentsHost, &IntentsModuleHost::pullWork, Qt::QueuedConnection);

    connect(m_intentsHost, &IntentsModuleHost::chooserRequested, this,
            [this](const QString& id, const QString& intent, const QString& requester,
                   const QVariantList& providers) {
                m_chooserQueue->request({id, intent, requester, providers});
            });
    connect(m_intentsHost, &IntentsModuleHost::chooserDismissed,
            m_chooserQueue, &IntentChooserQueue::withdraw);
    connect(m_intentsHost, &IntentsModuleHost::shellDeliveryEnded, this,
            [this](const QString& id) {
                if (m_packageCoordinator) m_packageCoordinator->shellIntentEnded(id);
                m_confirmations->withdrawn(id);
            });
    // Someone else may hold it now: it does not keep running.
    connect(m_intentsHost, &IntentsModuleHost::presenterRoleLost, this, [this]() {
        qWarning() << "intents_module could not be claimed again; unloading it";
        m_coreModuleManager->unloadModule(QStringLiteral("intents_module"));
    });
    connect(m_intentsHost, &IntentsModuleHost::installOffered, this,
            [this](const QString& intent) {
                // Basecamp's own requests fall back to the App Manager instead.
                if (ShellIntents::kShellUses.contains(intent)) return;
                const QStringList candidates = m_intentRegistry
                    ? m_intentRegistry->installableProvidersFor(intent) : QStringList{};
                if (candidates.isEmpty()) m_intentInstaller->nothingInstallable(intent);
                else m_intentInstaller->offerInstall(intent, candidates);
            });
    connect(m_uiPluginManager, &UIPluginManager::pluginLoadFailedNotice, this,
            [this](const QString& name, const QString&) { m_intentsHost->appLoadFailed(name); });
    connect(m_uiPluginManager, &UIPluginManager::missingDepsPopupRequested, this,
            [this](const QString& name, const QVariantList&, const QString&) {
                m_intentsHost->appLoadFailed(name);
            });
    connect(m_uiPluginManager, &UIPluginManager::appReady,
            m_intentsHost, &IntentsModuleHost::appReady);
    connect(m_uiPluginManager, &UIPluginManager::appBackendStopped, this,
            [this](const QString& name, QWidget*) { m_intentsHost->unregisterBackend(name); });
    connect(m_coreModuleManager, &CoreModuleManager::coreModulesChanged,
            this, &MainUIBackend::pushInstalledToIntentsModule);
    connect(m_coreModuleManager, &CoreModuleManager::coreModulesChanged,
            this, &MainUIBackend::keepIntentsModuleRunning);
}

void MainUIBackend::keepIntentsModuleRunning()
{
    if (!m_intentsHost->takeReload(
            m_coreModuleManager->loadedModules().contains(QStringLiteral("intents_module"))))
        return;
    qWarning() << "intents_module stopped; loading it again";
    QPointer<IntentsModuleHost> host(m_intentsHost);
    m_coreModuleManager->loadModuleInBackground(QStringLiteral("intents_module"), [host](bool loaded) {
        if (host) host->reloadFinished(loaded);
    });
}

// Apps and core modules alike. An app asks and answers through its backend,
// which goes in with the app's name and icon; the app itself is only a name there.
void MainUIBackend::pushInstalledToIntentsModule()
{
    if (!m_intentsHost || !m_uiPluginManager || !m_coreModuleManager) return;
    const QMap<QString, QVariantMap> plugins = m_uiPluginManager->uiPluginMetadataSnapshot();
    const QStringList known = m_coreModuleManager->knownModules();
    QJsonArray installed;
    for (auto it = plugins.cbegin(); it != plugins.cend(); ++it) {
        const QString installDir = it.value().value(QStringLiteral("installDir")).toString();
        const QString displayName = displayNameFor(it.key());
        const QString icon = m_uiPluginManager->pluginIconUrl(it.key());
        installed.append(QJsonObject{
            {QStringLiteral("name"), it.key()},
            {QStringLiteral("installDir"), installDir},
            {QStringLiteral("displayName"), displayName},
            {QStringLiteral("iconSource"), icon}});
        // From the metadata.json its app ships for it, under its name, whether
        // or not it ran yet: the core knows it only once the app first opens.
        // Not when an installed module has the name: that one runs instead
        // (PluginLoader refuses the app's), and is only itself.
        const QString backend = m_uiPluginManager->backendOfApp(it.key());
        if (backend.isEmpty() || (known.contains(backend) && !m_uiPluginManager->isAppBackend(backend)))
            continue;
        installed.append(QJsonObject{
            {QStringLiteral("name"), backend},
            {QStringLiteral("installDir"), QDir(installDir).filePath(backend)},
            {QStringLiteral("displayName"), displayName},
            {QStringLiteral("iconSource"), icon}});
    }
    if (m_core) {
        for (const QJsonValue& v : QJsonDocument::fromJson(m_core->modulesInfoJson().toUtf8()).array()) {
            const QJsonObject module = v.toObject();
            const QString name = module.value(QStringLiteral("name")).toString();
            if (name.isEmpty() || plugins.contains(name) || m_uiPluginManager->isAppBackend(name)) continue;
            const QFileInfo path(module.value(QStringLiteral("path")).toString());
            installed.append(QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("installDir"), path.isDir() ? path.absoluteFilePath() : path.absolutePath()},
                {QStringLiteral("displayName"), displayNameFor(name)}});
        }
    }
    m_intentsHost->setInstalled(installed);
}

void MainUIBackend::rebuildIntentRegistry()
{
    if (!m_intentRegistry || !m_uiPluginManager) return;
    pushInstalledToIntentsModule();
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

    // THE COLD-START GATE, and it must skip the bootstrap call above.
    //
    // A URL that launched the app has been sitting in the inbox since main().
    // Replaying it needs the registry to know what is INSTALLED, which the
    // constructor-time rebuild does not — its snapshot is empty because the
    // plugin scan has not run. Opening the gate there made shell intents work
    // (they are registered in code) while `basecamp://app/<name>` silently did
    // nothing: the name was absent from an empty snapshot, so it fell through
    // to the install branch and there was nothing to install it with either.
    //
    // uiPluginMetadataChanged is emitted unconditionally once the scan
    // completes — including with nothing installed, which is still a truthful
    // "here is what is on disk" — so this cannot strand a link on a clean
    // machine. The coordinator's park deadline remains the backstop.
    if (m_intentRegistryBootstrapped && m_linkRequests)
        m_linkRequests->onRegistryReady();
}

void MainUIBackend::rebuildInstallableProviders()
{
    if (!m_intentRegistry || !m_appsModel) return;

    QMap<QString, QStringList> byModule;
    QMap<QString, QStringList> webByModule;
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

        const QString name = idx.data(AppsModelRoles::NameRole).toString();
        byModule.insert(name, provides);
        const QStringList web = idx.data(AppsModelRoles::WebProvidesRole).toStringList();
        if (!web.isEmpty()) webByModule.insert(name, web);
    }
    // Runs on every apps-model change, download progress included; only a
    // changed list does anything.
    if (!m_intentRegistry->setInstallableProviders(byModule, webByModule))
        return;
    // An empty list is what made an install offer never appear, so say so.
    qCDebug(lcBasecampIntents).noquote()
        << "installable providers ="
        << (byModule.isEmpty() ? QStringLiteral("(none)")
                               : QStringList(byModule.keys()).join(QStringLiteral(", ")));
}

// ── Intent surface for QML — all delegating one-liners ──────────────────────

bool MainUIBackend::respondToShellIntent(const QString& requestId, bool ok,
                                         const QVariant& data, const QString& error)
{
    return answerShellIntent(requestId, ok, data, error, {});
}

bool MainUIBackend::answerShellIntent(const QString& requestId, bool ok, const QVariant& data,
                                      const QString& error, std::function<void(bool)> heard)
{
    // `data` is an untyped QVariant, so QML hands over the QJSValue wrapper
    // untouched — and a QJSValue is engine-bound, hence non-canonical, so the
    // broker would drop the response and mint `failed` at a caller that did
    // nothing wrong. LogosQmlBridge::respond flattens for the same reason on
    // the app path; this is the shell's copy of that boundary.
    const QVariant flat = data.userType() == qMetaTypeId<QJSValue>()
                        ? data.value<QJSValue>().toVariant()
                        : data;

    if (m_intentsHost && m_intentsHost->ownsShellDelivery(requestId))
        return m_intentsHost->respondShell(requestId, ok, flat, error, std::move(heard));
    const bool taken = m_intentBroker && m_shellEndpoint
        && m_intentBroker->submitResponse(m_shellEndpoint.get(), requestId, ok, flat, error);
    if (heard) heard(taken);
    return taken;
}

void MainUIBackend::resolveIntentChooser(const QString& dispatchId,
                                         const QString& providerName)
{
    if (m_intentsHost && m_intentsHost->ownsChoice(dispatchId))
        m_intentsHost->choose(dispatchId, providerName);
    else if (m_intentBroker)
        m_intentBroker->resolveChooser(dispatchId, providerName);
}

void MainUIBackend::cancelIntentChooser(const QString& dispatchId)
{
    if (m_intentsHost && m_intentsHost->ownsChoice(dispatchId))
        m_intentsHost->cancelChoice(dispatchId);
    else if (m_intentBroker)
        m_intentBroker->cancelChooser(dispatchId);
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

// Carry WHERE a candidate comes from. A package suggested by a repo the user
// added for something unrelated deserves to be recognised as such before it is
// installed. Shared by the install offer and the chooser's not-installed
// section, so the same package cannot be described two ways.
QVariantMap MainUIBackend::installCandidateDetails(const QString& name) const
{
    const QString repoUrl = repositoryUrlFor(name);
    const QVariantMap src = m_packageCoordinator
        ? m_packageCoordinator->repositorySource(repoUrl)
        : QVariantMap{};
    return QVariantMap{
        {QStringLiteral("moduleName"), name},
        {QStringLiteral("displayName"), displayNameFor(name)},
        {QStringLiteral("repositoryUrl"), repoUrl},
        {QStringLiteral("repositoryLabel"), src.value(QStringLiteral("label"))},
        {QStringLiteral("repositoryLink"),  src.value(QStringLiteral("link"))},
    };
}

// Catalog packages that could service `intent` and are NOT installed — the
// registry drops any entry already on disk, so this never overlaps the
// chooser's real providers.
QVariantList MainUIBackend::installableProvidersFor(const QString& intent) const
{
    if (!m_intentRegistry) return {};
    QVariantList out;
    for (const QString& name : m_intentRegistry->installableProvidersFor(intent))
        out.append(installCandidateDetails(name));
    return out;
}

// Raise the install offer for ONE package, for a user who picked it out of the
// chooser's not-installed section. The request it came from has already been
// answered `cancelled` by the dialog — installing resumes nothing, exactly as
// on the nothing-installed path.
void MainUIBackend::offerInstallFor(const QString& intent, const QString& moduleName)
{
    if (intent.isEmpty() || moduleName.isEmpty()) return;
    emit intentInstallOffered(intent, QStringList{moduleName},
                              QVariantList{installCandidateDetails(moduleName)});
}

QVariantMap MainUIBackend::providerDetailsFor(const QString& packageName) const
{
    QVariantMap out;
    out.insert(QStringLiteral("moduleName"), packageName);
    out.insert(QStringLiteral("displayName"), displayNameFor(packageName));

    // Basecamp's own requests and links come from no package.
    const bool aPackage = packageName != QLatin1String("main_ui")
                          && packageName != LinkRequestCoordinator::requesterName();
    if (m_packageCoordinator && aPackage) {
        out.insert(QStringLiteral("trust"), m_packageCoordinator->installedTrust(packageName));
        out.insert(QStringLiteral("installType"),
                   m_packageCoordinator->installType(packageName));
    }

    for (int row = 0; m_appsModel && row < m_appsModel->rowCount(); ++row) {
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

    if (m_packageCoordinator) {
        const QVariantMap src = m_packageCoordinator->repositorySource(
            out.value(QStringLiteral("repositoryUrl")).toString());
        out.insert(QStringLiteral("repositoryLabel"), src.value(QStringLiteral("label")));
        out.insert(QStringLiteral("repositoryLink"),  src.value(QStringLiteral("link")));
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
    QPointer<MainUIBackend> self(this);
    askInstallerApp(QStringLiteral("packages.show"), packageName, [self, packageName]() {
        if (self) self->showPackageDetailsFallback(packageName);
    });
}

void MainUIBackend::requestPackageInstall(const QString& packageName)
{
    if (packageName.isEmpty()) return;
    QPointer<MainUIBackend> self(this);
    askInstallerApp(QStringLiteral("packages.install"), packageName, [self]() {
        if (self) self->setCurrentActiveSectionIndex(ShellSection::AppManager);
    });
}

void MainUIBackend::searchApps(const QString& query)
{
    setCurrentActiveSectionIndex(ShellSection::AppManager);
    if (query.isEmpty() || !m_intentsHost) return;
    m_intentsHost->shellRequest(QStringLiteral("apps.search"), {{QStringLiteral("query"), query}},
                                /*asLink=*/false);
}

void MainUIBackend::askInstallerApp(const QString& intent, const QString& packageName,
                                    std::function<void()> orElse)
{
    const auto fallBack = [intent, packageName, orElse]() {
        qWarning().noquote() << QStringLiteral("intents_module did not take %1 for %2; showing its section")
                                    .arg(intent, packageName);
        orElse();
    };
    if (!m_intentsHost) {
        fallBack();
        return;
    }
    // Refused, nothing it knows provides it, or it is gone; a cancel is the
    // person's answer.
    m_intentsHost->shellRequest(
        intent, {{QStringLiteral("name"), packageName}}, /*asLink=*/false,
        [fallBack](const QVariantMap& result) {
            const QString error = result.value(QStringLiteral("error")).toString();
            if (error == QLatin1String("unavailable") || error == QLatin1String("failed")) fallBack();
        });
}

void MainUIBackend::intentChooserClosed(const QString& dispatchId)
{
    m_chooserQueue->closed(dispatchId);
}

void MainUIBackend::noticeBuiltInCopy(const QString& name, const QString& from, const QString& to,
                                      bool stopped)
{
    if (to.isEmpty()) {
        if (m_builtInCopiesLeft.contains(from))
            return;
        m_builtInCopiesLeft.insert(from);
    }
    const logos::builtin::SetAside copy{name, from, to, true, stopped};
    qWarning().noquote() << logos::builtin::setAsideMessage(copy);
    emit startupNoticeRequested(logos::builtin::setAsideTitle(copy), name,
                                logos::builtin::setAsideMessage(copy));
}

void MainUIBackend::offerInstallForUnknownApp(const QString& appName)
{
    if (appName.isEmpty() || !m_appsModel || !m_packageCoordinator)
        return;

    // The offer needs the app's catalog row, which may not be fetched yet.
    m_packageCoordinator->whenCatalogLoaded([this, appName]() {
        const QVariantMap row = m_appsModel->rowDataByName(appName, QString());
        if (row.isEmpty())
            return;

        const QVariantList detailed{ QVariantMap{
            { QStringLiteral("moduleName"),    appName },
            { QStringLiteral("displayName"),   displayNameFor(appName) },
            { QStringLiteral("repositoryUrl"), repositoryUrlFor(appName) },
        } };

        emit intentInstallOffered(kAppLaunchIntent, QStringList{ appName }, detailed);
    });
}

int MainUIBackend::beginAppLaunch(const QString& dispatchId,
                                  const QVariantMap& params)
{
    // The shell's `provides` carries no `params` — registerShellProvider takes
    // no spec — so nothing upstream has type-checked this. Validate here, and
    // treat a bad payload exactly like a missing app: same answer, same floor.
    // Reporting `bad_request` would leak that the name was well-formed but
    // absent, which is half the oracle back.
    const QVariant raw = params.value(kAppLaunchParam);
    const QString appName =
        raw.typeId() == QMetaType::QString ? raw.toString().trimmed() : QString();

    // ALWAYS ATTEMPT THE LAUNCH; the snapshot only decides whether to ALSO
    // offer an install.
    //
    // Gating the launch on the snapshot made this silently do nothing whenever
    // the snapshot was empty or stale — the name was absent, so it fell to the
    // install branch, and with nothing to install it with the click vanished.
    // onAppLauncherClicked/activateApp already no-op on a name they do not
    // recognise, so trying first and asking questions after is both simpler and
    // strictly harder to break.
    const bool known = m_uiPluginManager
                    && m_uiPluginManager->uiPluginMetadataSnapshot().contains(appName);

    const int appSection = m_uiPluginManager ? m_uiPluginManager->sectionOfApp(appName) : -1;
    if (appSection >= 0) {
        setCurrentActiveSectionIndex(appSection);
    } else if (!appName.isEmpty() && m_intentPresenter) {
        m_intentPresenter->ensureAppLoaded(appName);
        m_intentPresenter->presentApp(appName);

        if (!known)
            offerInstallForUnknownApp(appName);
    }

    constexpr int kLaunchAnswerFloorMs = 400;
    QTimer::singleShot(kLaunchAnswerFloorMs, this, [this, dispatchId]() {
        respondToShellIntent(dispatchId, true, QVariant(), QString());
    });

    return 1;
}

void MainUIBackend::showPackageDetailsFallback(const QString&)
{
    // No Package Manager to ask: intents_module, the only way it answers, is
    // not running. Its section still does something rather than nothing.
    setCurrentActiveSectionIndex(ShellSection::PackageManager);
}

void MainUIBackend::beginIntentInstall(const QString& providerName)
{
    // The normal install path: App Manager, then Basecamp's gate. Being
    // prompted by an intent buys no shortcut past the confirmation the user
    // would see anywhere else — it is the same install, started for a
    // different reason.
    requestPackageInstall(providerName);
}

QString MainUIBackend::displayNameFor(const QString& n) const {
    return m_packageCoordinator ? m_packageCoordinator->displayNameFor(n) : n;
}
void MainUIBackend::confirmUninstallCascade(const QString& n, const QStringList& optionalNames, const QVariantMap& optionalVersionPins, bool trustPublishers) { m_packageCoordinator->confirmUninstallCascade(n, optionalNames, optionalVersionPins, trustPublishers); }
void MainUIBackend::confirmUninstallMultiCascade(const QStringList& names) { m_packageCoordinator->confirmUninstallMultiCascade(names); }
void MainUIBackend::cancelMultiUninstall(const QStringList& names)         { m_packageCoordinator->cancelMultiUninstall(names); }
void MainUIBackend::refreshOptionalPreview(const QString& n, const QVariantMap& selection,
                                            const QVariantMap& optionalPins)
{ m_packageCoordinator->refreshOptionalPreview(n, selection, optionalPins); }
void MainUIBackend::confirmInstallGate(const QString& n, const QStringList& optionalNames, const QVariantMap& optionalVersionPins, bool trustPublishers) { m_packageCoordinator->confirmInstallGate(n, optionalNames, optionalVersionPins, trustPublishers); }
void MainUIBackend::cancelInstallGate(const QString& n)       { m_packageCoordinator->cancelInstallGate(n); }
void MainUIBackend::openApp(const QString& name, const QString& repositoryUrl, const QVariantMap& versionPins, bool allowFastLaunch)
{ m_packageCoordinator->openApp(name, repositoryUrl, versionPins, allowFastLaunch); }
void MainUIBackend::refreshTrustedPublishers()                  { m_packageCoordinator->refreshTrustedPublishers(); }
void MainUIBackend::removeTrustedPublisher(const QString& keyName) { m_packageCoordinator->removeTrustedPublisher(keyName); }
QStringList MainUIBackend::installedFromPublisher(const QString& did) const
{ return m_packageCoordinator->installedFromPublisher(did); }

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
QVariantList MainUIBackend::trustedPublishers() const   { return m_packageCoordinator->trustedPublishers(); }
bool         MainUIBackend::repositoriesLoading() const { return m_packageCoordinator->repositoriesLoading(); }
QString      MainUIBackend::downloadSource() const      { return m_packageCoordinator->downloadSource(); }
bool         MainUIBackend::onboardingRequired() const  { return m_packageCoordinator->onboardingRequired(); }
QString      MainUIBackend::storageState() const        { return m_packageCoordinator->storageState(); }
QString      MainUIBackend::storageError() const        { return m_packageCoordinator->storageError(); }
bool MainUIBackend::online() const
{
    // Test switch: 1 offline, 0 online (a sandbox has no network).
    const QByteArray forced = qgetenv("BASECAMP_FORCE_OFFLINE");
    if (forced == "1" || forced == "0") return forced == "0";
    const QNetworkInformation* info = QNetworkInformation::instance();
    if (!info) return true;
    const auto r = info->reachability();
    return r != QNetworkInformation::Reachability::Disconnected
        && r != QNetworkInformation::Reachability::Local;
}

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
void MainUIBackend::requestCatalog()                                       { m_packageCoordinator->requestCatalog(); }
void MainUIBackend::addRepository(const QString& url)                      { m_packageCoordinator->addRepository(url); }
void MainUIBackend::removeRepository(const QString& url)                   { m_packageCoordinator->removeRepository(url); }
void MainUIBackend::setRepositoryEnabled(const QString& url, bool enabled) { m_packageCoordinator->setRepositoryEnabled(url, enabled); }
void MainUIBackend::refreshDownloadSource()                                { m_packageCoordinator->refreshDownloadSource(); }
void MainUIBackend::setDownloadSource(const QString& source)               { m_packageCoordinator->setDownloadSource(source); }
void MainUIBackend::chooseOnboardingSource(const QString& source)          { m_packageCoordinator->chooseOnboardingSource(source); }
void MainUIBackend::completeOnboarding()                                    { m_packageCoordinator->completeOnboarding(); }
void MainUIBackend::retryStorage()                                         { m_packageCoordinator->retryStorage(); }

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
bool         MainUIBackend::packageCatalog() const { return m_hostProfile.packageCatalog; }
bool         MainUIBackend::repositoryManagement() const { return m_hostProfile.repositoryManagement; }
bool         MainUIBackend::devHost() const { return m_hostProfile.devHost; }

QVariantList MainUIBackend::availableSections() const
{
    QVariantList sections;
    for (int s : m_hostProfile.availableSections()) sections << s;
    return sections;
}

QVariantMap MainUIBackend::sectionApps() const
{
    QVariantMap apps;
    for (const QString& app : logos::defaultapps::apps()) {
        const int section = m_hostProfile.sectionOfApp(app);
        if (section >= 0) apps.insert(QString::number(section), app);
    }
    return apps;
}

// Read the environment rather than a compile-time flag.
bool         MainUIBackend::isMockBackend() const
{
    return qEnvironmentVariableIsSet("LOGOS_MOCK_FIXTURE");
}
QVariantList MainUIBackend::buildCommits() const    { return LogosBasecampBuildInfo::commits(); }

QString MainUIBackend::userDirectory() const
{
    return QDir::toNativeSeparators(LogosBasecampPaths::baseDirectory());
}

bool MainUIBackend::openUserDirectory() const
{
    return LogosBasecamp::revealInFileManager(LogosBasecampPaths::baseDirectory(),
                                              LogosBasecamp::currentFileManagerPlatform(),
                                              LogosBasecamp::systemFileManagerLauncher());
}


void MainUIBackend::setLinkRaiseHandler(std::function<void()> raise)
{
    if (m_linkRequests)
        m_linkRequests->setRaiseHandler(std::move(raise));
}
