#include "PackageCoordinator.h"
#include "AppsFilterProxy.h"
#include "AppsModel.h"
#include "CoreModuleManager.h"
#include "UIPluginManager.h"
#include "LogosBasecampPaths.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPointer>
#include <QTimer>

#include <memory>

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
{
    subscribeToPackageInstallationEvents();
    subscribeToPackageDownloaderEvents();

    // NB: initial metadata fetch is deferred until MainUIBackend calls
    // refresh() — the uiPluginsFetched signal would otherwise fire before
    // UIPluginManager's setPackageCoordinator runs and the slot connection
    // lands, causing the first-paint UI-plugin list to be empty until the
    // next file-install event triggers a re-scan.
}

PackageCoordinator::~PackageCoordinator() = default;

void PackageCoordinator::subscribeToPackageInstallationEvents()
{
    if (!m_logosAPI) {
        return;
    }

    LogosAPIClient* client = m_logosAPI->getClient("package_manager");
    if (!client || !client->isConnected()) {
        return;
    }

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
    logos.package_manager.on("beforeUninstall", [this](const QVariantList& data) {
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

    logos.package_manager.on("beforeUpgrade", [this](const QVariantList& data) {
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
        onBeforeUpgrade(name, releaseTag, mode, installedDeps);
    });

    // Multi-uninstall is a separate event so existing single-uninstall handlers
    // don't have to peek at the payload shape to disambiguate.
    logos.package_manager.on("beforeMultiUninstall", [this](const QVariantList& data) {
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
}

void PackageCoordinator::subscribeToPackageDownloaderEvents()
{
    if (!m_logosAPI) return;

    LogosAPIClient* client = m_logosAPI->getClient("package_downloader");
    if (!client || !client->isConnected()) return;

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

QStringList PackageCoordinator::dependentsOf(const QString& name) const
{
    return m_dependentsByModule.value(name);
}

// ---------------------------------------------------------------------------
// Install flow
// ---------------------------------------------------------------------------

void PackageCoordinator::openInstallPluginDialog()
{
    QString filter = "LGX Package (*.lgx);;All Files (*)";
    QString filePath = QFileDialog::getOpenFileName(nullptr, tr("Select Plugin to Install"), QString(), filter);
    if (!filePath.isEmpty()) {
        installPluginFromPath(filePath);
    }
}

void PackageCoordinator::installPluginFromPath(const QString& filePath)
{
    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);

    // Inspect the LGX first — no files are modified. The result tells us
    // whether the package is already installed and what depends on it, so
    // we can show the right confirmation dialog before doing anything
    // destructive.
    logos.package_manager.inspectPackageAsync(filePath,
        [self, filePath](QVariantMap info) {
            if (!self) return;
            if (!info.value("error").toString().isEmpty()) {
                qWarning() << "Failed to inspect LGX:" << info.value("error").toString();
                return;
            }

            self->m_pendingInstallPath = filePath;
            const bool isAlreadyInstalled = info.value("isAlreadyInstalled").toBool();

            if (isAlreadyInstalled) {
                const QString name = info.value("name").toString();
                const QStringList deps = info.value("installedDependents").toStringList();
                const QStringList loaded = self->m_uiPluginManager
                    ? self->m_uiPluginManager->intersectWithLoaded(deps)
                    : QStringList{};

                if (!loaded.isEmpty() || !deps.isEmpty()) {
                    // Upgrade with dependents — set cascade pending state so
                    // confirmInstall routes through the cascade-unload path.
                    self->m_pendingAction = {PendingOp::InstallUpgradeCascade,
                                             name, {}, 0};
                    // Augment the metadata with loaded-deps so the install-
                    // confirm dialog can render both package details AND the
                    // dependents lists in one unified view.
                    info["loadedDependents"] = QVariant::fromValue(loaded);
                }
            }

            // Always show the install-confirm dialog — it adapts its layout
            // based on isAlreadyInstalled, installedDependents, and
            // loadedDependents fields in the metadata.
            emit self->installConfirmationRequested(info);
        });
}

void PackageCoordinator::confirmInstall()
{
    if (m_pendingInstallPath.isEmpty()) return;

    // Upgrade with dependents — the install-confirm dialog showed both
    // package metadata AND the cascade lists. Route to the cascade path
    // which handles unloading dependents → uninstall old → install new.
    if (m_pendingAction.op == PendingOp::InstallUpgradeCascade) {
        const QString name = m_pendingAction.name;
        confirmUninstallCascade(name);
        return;
    }

    // Simple install (fresh or upgrade without dependents).
    const QString path = m_pendingInstallPath;
    m_pendingInstallPath.clear();

    if (!m_logosAPI) return;
    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_manager.installPluginAsync(path, false,
        [self](QVariant result) {
            if (!self) return;
            Q_UNUSED(result);
            if (self->m_coreModuleManager) self->m_coreModuleManager->refresh();
            self->fetchUiPluginMetadata();
        });
}

void PackageCoordinator::cancelInstall()
{
    // Upgrade with dependents — clear the cascade pending state too.
    if (m_pendingAction.op == PendingOp::InstallUpgradeCascade) {
        m_pendingAction = {};
    }
    m_pendingInstallPath.clear();
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
            if (!result.value("success", false).toBool()) {
                qWarning() << "requestUninstall rejected for" << moduleName << ":"
                           << result.value("error").toString();
            }
        });
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
            if (!result.value("success", false).toBool()) {
                qWarning() << "requestUninstall rejected for" << moduleName << ":"
                           << result.value("error").toString();
            }
        });
}

// ---------------------------------------------------------------------------
// Cascade confirmation — triggered from QML once the user OKs the dialog.
// ---------------------------------------------------------------------------

void PackageCoordinator::confirmUninstallCascade(const QString& moduleName)
{
    if ((m_pendingAction.op != PendingOp::UninstallCascade &&
         m_pendingAction.op != PendingOp::UpgradeCascade &&
         m_pendingAction.op != PendingOp::InstallUpgradeCascade)
        || m_pendingAction.name != moduleName) {
        qWarning() << "confirmUninstallCascade for" << moduleName
                   << "but pending action is" << m_pendingAction.name;
        return;
    }

    // Snapshot before clearing — the callbacks below capture by value.
    const bool    isUpgrade         = (m_pendingAction.op == PendingOp::UpgradeCascade);
    const bool    isInstallUpgrade  = (m_pendingAction.op == PendingOp::InstallUpgradeCascade);
    const QString releaseTag        = m_pendingAction.releaseTag;
    const QString installPath       = m_pendingInstallPath;
    m_pendingAction = {};
    m_pendingInstallPath.clear();

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
    QPointer<PackageCoordinator> self(this);
    if (isInstallUpgrade) {
        // Local LGX file upgrade — uninstall old, then install new from file.
        // Two chained async calls: uninstallPackage removes the old files,
        // then installPlugin extracts the new LGX in its place.
        logos.package_manager.uninstallPackageAsync(moduleName,
            [self, moduleName, installPath](QVariantMap r) {
                if (!self) return;
                if (!r.value("success", false).toBool()) {
                    qWarning() << "Uninstall before local upgrade failed for"
                               << moduleName << ":" << r.value("error").toString();
                    return;
                }
                LogosModules logos2(self->m_logosAPI);
                logos2.package_manager.installPluginAsync(installPath, false,
                    [self](QVariant result) {
                        if (!self) return;
                        Q_UNUSED(result);
                        if (self->m_coreModuleManager) self->m_coreModuleManager->refresh();
                        self->fetchUiPluginMetadata();
                    });
            });
    } else if (isUpgrade) {
        // Catalog-based upgrade — the module does uninstall + emits
        // upgradeUninstallDone for PMU to drive download+install.
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
}

void PackageCoordinator::refresh()
{
    fetchUiPluginMetadata();
    refreshRepositories();
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
    m_pendingInstallPath.clear();   // safe no-op when no install is pending

    // InstallUpgradeCascade is local — just drop the pending slot and
    // we're done. It's driven entirely by inspectPackage + the ungated
    // uninstallPackage/installPlugin calls, so the module has no pending
    // action to cancel.
    //
    // Uninstall / Upgrade are gated by the module — tell it we bailed;
    // otherwise its pending slot stays set and the next request is
    // rejected with "another <op> is in progress".
    if (op == PendingOp::InstallUpgradeCascade) return;
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
            const QStringList loadedDeps = self->m_uiPluginManager
                ? self->m_uiPluginManager->intersectWithLoaded(installedDeps)
                : QStringList{};
            self->m_pendingAction = {PendingOp::UninstallCascade, name, QString{}, 0};
            emit self->uninstallCascadeConfirmationRequested(name, installedDeps, loadedDeps);
        });
}

void PackageCoordinator::onBeforeUpgrade(const QString& name, const QString& releaseTag,
                                     int mode, const QStringList& installedDeps)
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
        [self, name, releaseTag, mode, installedDeps](QVariantMap result) {
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
                name, releaseTag, mode, installedDeps, loadedDeps);
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
                return;
            }
            const QStringList loadedDeps = self->m_uiPluginManager
                ? self->m_uiPluginManager->intersectWithLoaded(installedDeps)
                : QStringList{};
            self->m_pendingAction = {PendingOp::MultiUninstallCascade, QString{}, QString{}, 0, names};
            emit self->uninstallMultiCascadeConfirmationRequested(names, installedDeps, loadedDeps);
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
    if (!m_logosAPI) return;

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
            emit selfPtr->repositoryOperationCompleted(operation, url,
                r.value("success").toBool(), r.value("error").toString());
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
        self->m_installedPackagesCache = packages;
        QMap<QString, QString>   typeMap;
        QSet<QString>            nameSet;
        QHash<QString, QString>  versionByName;
        QHash<QString, QString>  hashByName;
        nameSet.reserve(packages.size());
        versionByName.reserve(packages.size());
        hashByName.reserve(packages.size());
        for (const QVariant& v : packages) {
            const QVariantMap pkg = v.toMap();
            const QString name = pkg.value("name").toString();
            if (name.isEmpty()) continue;
            typeMap[name] = pkg.value("installType").toString();
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
        self->m_installTypeByModule    = typeMap;
        self->m_installedNameSet       = std::move(nameSet);
        self->m_installedVersionByName = std::move(versionByName);
        for (auto it = hashByName.cbegin(); it != hashByName.cend(); ++it) {
            self->m_installedHashByName.insert(it.key(), it.value());
        }

        // Replay markInstalled across the full package set — fetchUiPluginMetadata
        // only saw UI plugins. Idempotent on (version, hash).
        if (self->m_appsModel) {
            self->m_appsModel->beginBulkInstalledUpdate();
            for (auto it = self->m_installedVersionByName.cbegin();
                 it != self->m_installedVersionByName.cend(); ++it) {
                self->m_appsModel->markInstalled(
                    it.key(), it.value(),
                    self->m_installedHashByName.value(it.key()));
            }
            self->m_appsModel->endBulkInstalledUpdate();
        }

        // Second pass — per-module missing/dependents queries. Dispatched
        // for every entry in the full installed-packages list (both UI and
        // core) so that QML lookups work uniformly regardless of which tab
        // is surfacing the button.
        QStringList names = typeMap.keys();
        if (names.isEmpty()) {
            self->m_missingDepsByModule.clear();
            self->m_dependentsByModule.clear();
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
        auto dependentsMap = std::make_shared<QMap<QString, QStringList>>();
        QPointer<PackageCoordinator> selfCopy(self.data());

        auto maybeFinish = [selfCopy, missingMap, dependentsMap, remaining]() {
            if (!selfCopy) return;
            if (--(*remaining) > 0) return;
            selfCopy->m_missingDepsByModule = *missingMap;
            selfCopy->m_dependentsByModule = *dependentsMap;
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
            // Derive the missing-deps list client-side from the flat forward projection:
            // every node with status == "not_installed" is a missing dep.
            inner.package_manager.resolveFlatDependenciesAsync(
                name, true, [missingMap, name, maybeFinish](QVariantList deps) {
                    QStringList out;
                    for (const QVariant& v : deps) {
                        const QVariantMap m = v.toMap();
                        if (m.value("status").toString() != "not_installed") continue;
                        const QString s = m.value("name").toString();
                        if (!s.isEmpty()) out << s;
                    }
                    missingMap->insert(name, out);
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

void PackageCoordinator::setSessionStage(const QString& name, InstallStage::Value stage)
{
    auto it = m_installSessions.find(name);
    if (it == m_installSessions.end()) return;
    if (it->stage == stage) return;
    it->stage = stage;
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

void PackageCoordinator::runResolverAndOpenDialog(const QString& name,
                                                  const QString& repositoryUrl,
                                                  const QVariantMap& versionPins)
{
    QVariantMap catalogRow =
        m_appsModel ? m_appsModel->rowDataByName(name, repositoryUrl) : QVariantMap{};

    const QString targetVersion = versionPins.value(name).toString();

    const int epoch = ++m_dialogResolveEpoch[name];

    const QString depsJson = buildResolverDepsJson(name, repositoryUrl, versionPins);

    qDebug() << "PackageCoordinator::runResolverAndOpenDialog" << name
             << "repo=" << repositoryUrl << "targetVersion=" << targetVersion
             << "pins=" << versionPins.size() << "epoch=" << epoch;

    emitDialogMetadata(name, repositoryUrl, targetVersion, catalogRow, /*changes=*/{});

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
            self->emitDialogMetadata(name, repositoryUrl, targetVersion, catalogRow, changes);
        });
}

void PackageCoordinator::emitDialogMetadata(const QString& name,
                                            const QString& repositoryUrl,
                                            const QString& targetVersion,
                                            const QVariantMap& catalogRow,
                                            const QVariantList& changes)
{
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
    const QVariantList versionsList = catalogRow.value("versions").toList();
    metadata["latestVersion"] = versionsList.isEmpty()
        ? QString()
        : versionsList.first().toMap().value("manifest").toMap().value("version").toString();

    metadata["installStage"] = static_cast<int>(
        m_installSessions.contains(name)
            ? m_installSessions.value(name).stage
            : InstallStage::None);

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

    if (m_requiredPackagesModel)
        m_requiredPackagesModel->setRequiredPackages(requiredEntries);

    emit addApplicationRequested(metadata);
}

void PackageCoordinator::confirmCatalogInstall(const QString& name,
                                                const QString& repositoryUrl,
                                                const QVariantMap& versionPins)
{
    if (!m_logosAPI || name.isEmpty()) return;

    if (m_installSessions.contains(name)) {
        qDebug() << "confirmCatalogInstall: session for" << name
                 << "already in progress, ignoring";
        return;
    }

    InstallSession s;
    s.name  = name;
    s.stage = InstallStage::Downloading;
    m_installSessions.insert(name, s);

    if (m_appsModel) m_appsModel->setInstallStage(name, InstallStage::Downloading);

    const QString depsJson = buildResolverDepsJson(name, repositoryUrl, versionPins);

    LogosModules logos(m_logosAPI);
    QPointer<PackageCoordinator> self(this);
    logos.package_downloader.downloadResolvedDependenciesAsync(depsJson,
        [self, name](QVariantList results) {
            if (!self) return;

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
                    if (self->m_appsModel)
                        self->m_appsModel->setInstallStage(rowName,
                            InstallStage::Installed);
                    continue;
                }
                toInstall.append(v);
            }

            if (toInstall.isEmpty()) {
                // Nothing left to do after the skip-already-installed
                // filter; treat as a successful no-op rather than Failed.
                self->setSessionStage(name, InstallStage::Installed);
                emit self->catalogInstallFinished(name);
                QTimer::singleShot(1500, self.data(), [self, name]() {
                    if (!self) return;
                    self->m_installSessions.remove(name);
                });
                return;
            }

            for (const QVariant& v : toInstall) {
                const QString rowName = v.toMap().value("name").toString();
                if (!rowName.isEmpty() && self->m_appsModel)
                    self->m_appsModel->setInstallStage(rowName,
                        InstallStage::Queued);
            }

            self->setSessionStage(name, InstallStage::Installing);
            self->installResultsSequential(toInstall, name, 0);
        });
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
    if (!rowName.isEmpty() && m_appsModel)
        m_appsModel->setInstallStage(rowName, InstallStage::Installing);

    QPointer<PackageCoordinator> self(this);
    installOnePackage(dl,
        [self, results, topLevelName, rowName, index, failures]
        (bool success, const QString& err) mutable {
            qDebug() << "installOnePackage callback rowName=" << rowName
                     << "success=" << success << "err=" << err;
            if (!self) return;

            if (!rowName.isEmpty()) {
                const InstallStage::Value stage = success
                    ? InstallStage::Installed
                    : InstallStage::Failed;
                if (self->m_appsModel)
                    self->m_appsModel->setInstallStage(rowName, stage,
                        success ? QString() : err);
            }
            if (!success) {
                failures.append(rowName.isEmpty()
                                    ? err
                                    : (rowName + ": " + err));
            }

            const bool isLast = (index + 1) >= results.size();
            if (!isLast) {
                self->installResultsSequential(
                    results, topLevelName, index + 1, failures);
                return;
            }

            if (!failures.isEmpty()) {
                qDebug() << "  install loop complete with failures for"
                         << topLevelName << ":" << failures.size();
                self->setSessionStage(topLevelName, InstallStage::Failed);
                emit self->catalogInstallFailed(
                    topLevelName, failures.join(QStringLiteral("; ")));
                QTimer::singleShot(2500, self.data(), [self, topLevelName]() {
                    if (!self) return;
                    self->m_installSessions.remove(topLevelName);
                });
                return;
            }

            self->setSessionStage(topLevelName, InstallStage::Installed);
            emit self->catalogInstallFinished(topLevelName);
            QTimer::singleShot(1500, self.data(), [self, topLevelName]() {
                if (!self) return;
                self->m_installSessions.remove(topLevelName);
            });
        });
}
