#include "PackageCoordinator.h"
#include "CoreModuleManager.h"
#include "UIPluginManager.h"
#include "LogosBasecampPaths.h"

#include <QDebug>
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
                               QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
    , m_coreModuleManager(coreModuleManager)
    , m_uiPluginManager(uiPluginManager)
{
    subscribeToPackageInstallationEvents();

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
            self->m_pendingAction = {PendingOp::UpgradeCascade, name, releaseTag, mode};
            // Reuse the uninstall cascade dialog — the copy applies verbatim
            // ("these things depend on X; X is about to go away"). Phase B
            // intentionally collapses upgrade/uninstall into one dialog.
            emit self->uninstallCascadeConfirmationRequested(name, installedDeps, loadedDeps);
        });
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
        for (const QVariant& item : uiPlugins) {
            QVariantMap pluginInfo = item.toMap();
            const QString name = pluginInfo.value("name").toString();
            if (name.isEmpty()) continue;
            self->m_installTypeByModule[name] = pluginInfo.value("installType").toString();
        }

        // Push the raw UI-plugin list to UIPluginManager — that's where the
        // UI-specific cache (used for widget loading) lives.
        emit self->uiPluginsFetched(uiPlugins);
        emit self->uiModulesChanged();
        emit self->launcherAppsChanged();

        // Kick off the dependency-info refresh. This is asynchronous and
        // touches every known package, so it may emit uiModulesChanged()
        // again once it completes.
        self->refreshDependencyInfo();
    });
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
        QMap<QString, QString> typeMap;
        for (const QVariant& v : packages) {
            const QVariantMap pkg = v.toMap();
            const QString name = pkg.value("name").toString();
            if (name.isEmpty()) continue;
            typeMap[name] = pkg.value("installType").toString();
        }
        self->m_installTypeByModule = typeMap;

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
