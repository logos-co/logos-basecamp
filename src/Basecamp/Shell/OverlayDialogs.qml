import QtQuick
import QtQuick.Controls
import Logos.Theme
import Logos.Controls
import Basecamp.AppManager
import Basecamp.Backend 1.0

// Global dialog layer hosted in a transparent top-level QQuickWidget
// (MainContainer::m_overlayWidget). Keeps the ContentViews-scoped layer
// from being the only place these dialogs can render — the user can now
// click a plugin icon in the sidebar while on the Apps/workspace screen
// and get the missing-deps popup, which previously needed them to
// navigate to Modules first.
//
// Why not just move these from ContentViews.qml? The contentStack swaps
// between WorkspaceArea (C++ widget) and the ContentViews QQuickWidget —
// when the workspace is showing, ContentViews is hidden, and its Dialog
// children never render. An independent overlay widget sidesteps the
// stacking entirely.
//
// The overlay widget is kept visible all the time with
// WA_TransparentForMouseEvents so the sidebar + content below can still
// receive input. anyDialogOpen flips that flag off while a dialog is
// modal, then back on when it closes.
Item {
    id: root

    // Stable handle for UI automation (doc-tests drive this layer through the
    // QML inspector's findByProperty/callMethod).
    objectName: "overlayDialogs"

    // True iff any dialog is currently visible. Drives input-blocking
    // (WA_TransparentForMouseEvents flip) on the hosting QQuickWidget
    // — see MainContainer::onOverlayActiveChanged.
    property bool anyDialogOpen: missingDepsDialog.visible
                                  || unloadCascadeDialog.visible
                                  || uninstallDialog.visible
                                  || upgradeCascadeDialog.visible
                                  || installGateDialog.visible
                                  || installErrorDialog.visible
                                  || addApplicationDialog.visible
    property string sidebarTooltipText: ""
    property real   sidebarTooltipY:    0

    signal overlayActiveChanged(bool active)

    onAnyDialogOpenChanged: root.overlayActiveChanged(anyDialogOpen)

    QtObject {
        id: _dialogDeps
        property var displayNameLookup: function(name) { return backend.displayNameFor(name); }
    }

    // Each dialog instance carries a mode-derived objectName (matching the
    // button convention inside ConfirmationDialog) so UI automation can
    // assert WHICH dialog is open via its `visible` property. Text-based
    // assertions alone can't: the per-mode instances keep their constant
    // titles — and whatever body text they last rendered — in the object
    // tree even while closed.
    ConfirmationDialog {
        id: missingDepsDialog
        objectName: "confirmationDialog.missingDeps"
        mode: "missingDeps"
        displayNameLookup: _dialogDeps.displayNameLookup
    }

    ConfirmationDialog {
        id: unloadCascadeDialog
        objectName: "confirmationDialog.unloadCascade"
        mode: "unloadCascade"
        displayNameLookup: _dialogDeps.displayNameLookup
        onContinueClicked: (name) => backend.confirmUnloadCascade(name)
        onCancelClicked: (name) => backend.cancelPendingAction(name)
    }

    // The one uninstall confirmation — all four initiators arrive here.
    UninstallDialog {
        id: uninstallDialog
        objectName: "uninstallDialog"
        onConfirmed: function(plan) {
            if (plan.multi) backend.confirmUninstallMultiCascade(plan.batch)
            else            backend.confirmUninstallCascade(plan.batch[0])
        }
        onCancelled: function(plan) {
            // Real-plan cancel — a module request is in flight.
            if (plan.multi) backend.cancelMultiUninstall(plan.batch)
            else            backend.cancelPendingAction(plan.batch[0])
        }
        onCancelledWhileLoading: function(plan) {
            // No module IPC has fired yet; just drop the local pending name.
            backend.cancelPendingUninstallApp(plan.targetName)
        }
    }

    // Distinct dialog instance for upgrade/downgrade/reinstall cascades so
    // the title + body can lead with the target version + UpgradeMode
    // instead of "Uninstall and Unload Dependents?" (the previous
    // shared-with-uninstall dialog confused users on downgrades — see
    // PackageCoordinator::onBeforeUpgrade for the rationale). Confirm/
    // Cancel routes through the same backend slots; PackageCoordinator
    // disambiguates from its own m_pendingAction.op
    // (UpgradeCascade vs UninstallCascade).
    ConfirmationDialog {
        id: upgradeCascadeDialog
        objectName: "confirmationDialog.upgradeCascade"
        mode: "upgradeCascade"
        displayNameLookup: _dialogDeps.displayNameLookup
        onContinueClicked: (name) => backend.confirmUninstallCascade(name)
        onCancelClicked: (name) => backend.cancelPendingAction(name)
    }

    // Install gate initiated by package_manager_ui (via the module's
    // requestInstall) — the only install confirmation in the app, covering
    // both catalog downloads and local .lgx picks. Confirm/cancel forward the
    // decision back through the module gate so PMU installs (or aborts).
    // Lists the resolved transitive dep changes.
    ConfirmationDialog {
        id: installGateDialog
        objectName: "confirmationDialog.installGate"
        mode: "installGate"
        displayNameLookup: _dialogDeps.displayNameLookup
        onContinueClicked: (name) => backend.confirmInstallGate(name)
        onCancelClicked: (name) => backend.cancelInstallGate(name)
    }

    // Informational — no confirm/cancel wiring, OK just closes.
    ConfirmationDialog {
        id: installErrorDialog
        objectName: "confirmationDialog.installError"
        mode: "installError"
        displayNameLookup: _dialogDeps.displayNameLookup
    }

    // The "Required Packages" view of the catalog, restricted to whatever the
    // resolver last returned. requiredPackageEntries is BOUND to the backend
    // property, replacing a host-side setRequiredPackages() write into an
    // object the host should not have been holding.
    AppsFilterProxy {
        id: requiredPackagesProxy
        sourceModel:            backend.appsModel
        excludeMainUi:          false
        installStateFilter:     ""
        requiredPackageEntries: backend.requiredPackages
    }

    // App-Manager "Add Application" dialog.
    AddApplicationDialog {
        id: addApplicationDialog
        requiredPackagesModel: requiredPackagesProxy
        onClosed: backend.notifyAddApplicationDialogClosed()
        onUninstallRequested: function(name, repositoryUrl) {
            backend.uninstallApp(name, repositoryUrl)
        }
        onInstallRequested: function(name, repositoryUrl, versionPins) {
            addApplicationDialog.installStage = InstallStage.Downloading
            backend.confirmCatalogInstall(name, repositoryUrl, versionPins)
        }
        onLaunchRequested: function(name) {
            backend.onAppLauncherClicked(name)
        }
        onVersionChangeRequested: function(name, repositoryUrl, versionPins) {
            backend.openApp(name, repositoryUrl, versionPins, false)
        }
    }

    LogosToolTip {
        id: sidebarTip
        parent: root
        text: root.sidebarTooltipText
        visible: text !== ""
        delay: 0
        placement: LogosToolTip.Right
        manualX: 68
        manualY: root.sidebarTooltipY - height / 2
    }


    Connections {
        target: backend
        ignoreUnknownSignals: true

        // `blockers` carries a reason per entry and `summary` names the set
        // as a whole — see UIPluginManager::missingDepsPopupRequested.
        function onMissingDepsPopupRequested(name, blockers, summary) {
            missingDepsDialog.openWith("missingDeps", name, blockers, summary);
        }

        function onUnloadCascadeConfirmationRequested(name, loadedDependents) {
            unloadCascadeDialog.openWith("unloadCascade", name, loadedDependents);
        }

        // Everything the popup renders — what's going, what's staying and
        // why, what breaks — is already resolved in the payload. See
        // PackageCoordinator::buildPlanPayload.
        function onUninstallPlanRequested(plan) {
            uninstallDialog.openWithPlan(plan);
        }

// Upgrade cascade: same dependent-impact shape as uninstall (the
        // package_manager performs an uninstall step first), but carries
        // the target version + UpgradeMode so the dialog can lead with
        // "Upgrade to vX.Y.Z" / "Downgrade to vX.Y.Z" / "Reinstall vX.Y.Z"
        // instead of a bare uninstall heading.
        function onUpgradeCascadeConfirmationRequested(name, releaseTag, mode,
                                                       installedDependents, loadedDependents,
                                                       depChanges) {
            upgradeCascadeDialog.openWithUpgrade(name, releaseTag, mode,
                                                 installedDependents, loadedDependents,
                                                 depChanges);
        }

        // Install gate (package_manager_ui-initiated). releaseTag is the
        // target version; depChanges is the resolved transitive set.
        function onInstallGateConfirmationRequested(name, releaseTag, depChanges) {
            installGateDialog.openWithInstallGate(name, releaseTag, depChanges);
        }

        function onInstallFailureNoticeRequested(name, errorMessage) {
            installErrorDialog.openWithInstallError(name, errorMessage);
        }

        function onLaunchAppRequested(name) {
            backend.onAppLauncherClicked(name);
        }

        function onRequestOpenAddApplicationDialog(metadata) {
            if (!addApplicationDialog.visible) {
                addApplicationDialog.openWith(metadata);
            } else if (addApplicationDialog.metadata.name === metadata.name) {
                // Version re-resolve while the same app's dialog is already open.
                addApplicationDialog.metadata = metadata;
                addApplicationDialog.installStage = metadata.installStage || InstallStage.None;
            }
        }

        function onAddApplicationDataUpdated(metadata) {
            if (!addApplicationDialog.visible) return;
            if (addApplicationDialog.metadata.name !== metadata.name) return;
            addApplicationDialog.metadata = metadata;
            addApplicationDialog.installStage = metadata.installStage || InstallStage.None;
        }

        function onCatalogInstallStageChanged(name, stage) {
            if (!addApplicationDialog.visible) return;
            if (addApplicationDialog.metadata.name !== name) return;
            addApplicationDialog.installStage = stage;
        }
        // Capture the failure reason so the dialog can show why the install failed.
        function onCatalogInstallFailed(name, error) {
            if (!addApplicationDialog.visible) return;
            if (addApplicationDialog.metadata.name !== name) return;
            addApplicationDialog.installStage = InstallStage.Failed;
            addApplicationDialog.installError = error;
        }
        function onCatalogInstallFinished(name) {
            if (addApplicationDialog.visible
                && addApplicationDialog.metadata.name === name) {
                addApplicationDialog.markInstallComplete();
            }
        }
    }
}
