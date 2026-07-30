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

    // True iff any dialog is currently visible. Drives input-blocking
    // (WA_TransparentForMouseEvents flip) on the hosting QQuickWidget
    // — see MainContainer::onOverlayActiveChanged.
    property bool anyDialogOpen: missingDepsDialog.visible
                                  || unloadCascadeDialog.visible
                                  || uninstallCascadeDialog.visible
                                  || upgradeCascadeDialog.visible
                                  || installConfirmDialog.visible
                                  || installGateDialog.visible
                                  || addApplicationDialog.visible
    property string sidebarTooltipText: ""
    property real   sidebarTooltipY:    0

    signal overlayActiveChanged(bool active)

    onAnyDialogOpenChanged: root.overlayActiveChanged(anyDialogOpen)

    QtObject {
        id: _dialogDeps
        property var displayNameLookup: function(name) { return backend.displayNameFor(name); }
    }

    ConfirmationDialog {
        id: missingDepsDialog
        mode: "missingDeps"
        displayNameLookup: _dialogDeps.displayNameLookup
    }

    ConfirmationDialog {
        id: unloadCascadeDialog
        mode: "unloadCascade"
        displayNameLookup: _dialogDeps.displayNameLookup
        onContinueClicked: (name) => backend.confirmUnloadCascade(name)
        onCancelClicked: (name) => backend.cancelPendingAction(name)
    }

    ConfirmationDialog {
        id: uninstallCascadeDialog
        mode: "uninstallCascade"
        displayNameLookup: _dialogDeps.displayNameLookup
        onContinueClicked: (name) => backend.confirmUninstallCascade(name)
        onCancelClicked: (name) => backend.cancelPendingAction(name)
        onContinueClickedMulti: (names) => backend.confirmUninstallMultiCascade(names)
        onCancelClickedMulti: (names) => backend.cancelMultiUninstall(names)
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
        mode: "upgradeCascade"
        displayNameLookup: _dialogDeps.displayNameLookup
        onContinueClicked: (name) => backend.confirmUninstallCascade(name)
        onCancelClicked: (name) => backend.cancelPendingAction(name)
    }

    ConfirmationDialog {
        id: installConfirmDialog
        mode: "installConfirm"
        displayNameLookup: _dialogDeps.displayNameLookup
        onContinueClicked: backend.confirmInstall()
        onCancelClicked: backend.cancelInstall()
    }

    // Fresh catalog-install gate initiated by package_manager_ui (via the
    // module's requestInstall). Distinct from installConfirmDialog (local-LGX
    // inspect flow): confirm/cancel forward the decision back through the
    // module gate so PMU downloads+installs (or aborts). Lists the resolved
    // transitive dep changes so this is the single install confirmation.
    ConfirmationDialog {
        id: installGateDialog
        mode: "installGate"
        displayNameLookup: _dialogDeps.displayNameLookup
        onContinueClicked: (name) => backend.confirmInstallGate(name)
        onCancelClicked: (name) => backend.cancelInstallGate(name)
    }

    // App-Manager "Add Application" dialog.
    AddApplicationDialog {
        id: addApplicationDialog
        requiredPackagesModel: backend.requiredPackagesModel
        onClosed: backend.notifyAddApplicationDialogClosed()
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

        function onMissingDepsPopupRequested(name, missing) {
            missingDepsDialog.openWith("missingDeps", name, missing);
        }

        function onUnloadCascadeConfirmationRequested(name, loadedDependents) {
            unloadCascadeDialog.openWith("unloadCascade", name, loadedDependents);
        }

        // Uninstall cascade gets TWO lists:
        //  * installedDependents — everything installed that depends on
        //    `name`, recursively. These will structurally break (fail to load
        //    next time) after the uninstall.
        //  * loadedDependents — subset currently loaded. These get torn down
        //    now as part of the cascade.
        // Both are rendered so the user can see full impact + immediate impact.
        function onUninstallCascadeConfirmationRequested(name, installedDependents, loadedDependents) {
            uninstallCascadeDialog.openWithTwoLists("uninstallCascade", name,
                                                    installedDependents, loadedDependents);
        }

        function onUninstallMultiCascadeConfirmationRequested(names, installedDependents, loadedDependents) {
            uninstallCascadeDialog.openWithMultiTargets(names, installedDependents, loadedDependents);
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

        function onInstallConfirmationRequested(metadata) {
            installConfirmDialog.openWithMetadata(metadata);
        }

        // Fresh catalog-install gate (package_manager_ui-initiated). releaseTag
        // is the target version; depChanges is the resolved transitive set.
        function onInstallGateConfirmationRequested(name, releaseTag, depChanges) {
            installGateDialog.openWithInstallGate(name, releaseTag, depChanges);
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
