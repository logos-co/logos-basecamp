import QtQuick
import QtQuick.Controls
import Logos.Theme
import Logos.Controls
import Basecamp.Controls
import Basecamp.Popups
import Basecamp.Backend 1.0

// Global dialog layer hosted in a transparent top-level QQuickWidget
// (MainContainer::m_overlayWidget). Keeps the ContentViews-scoped layer
// from being the only place these dialogs can render — the user can now
// click a plugin icon in the sidebar while on the Apps/MDI screen and
// get the missing-deps popup, which previously needed them to navigate
// to Modules first.
//
// Why not just move these from ContentViews.qml? The contentStack swaps
// between MdiView (C++ widget) and the ContentViews QQuickWidget — when
// MDI is showing, ContentViews is hidden, and its Dialog children never
// render. An independent overlay widget sidesteps the stacking entirely.
//
// The overlay widget is kept visible all the time with
// WA_TransparentForMouseEvents so the sidebar + content below can still
// receive input. anyDialogOpen flips that flag off while a dialog is
// modal, then back on when it closes.
Item {
    id: root

    // True iff any dialog is currently visible. C++ watches this via the
    // overlayActiveChanged signal and toggles mouse-event passthrough on
    // the hosting QQuickWidget.
    property bool anyDialogOpen: missingDepsDialog.visible
                                  || unloadCascadeDialog.visible
                                  || uninstallCascadeDialog.visible
                                  || upgradeCascadeDialog.visible
                                  || installConfirmDialog.visible
                                  || addApplicationDialog.visible

    signal overlayActiveChanged(bool active)

    onAnyDialogOpenChanged: root.overlayActiveChanged(anyDialogOpen)

    ConfirmationDialog {
        id: missingDepsDialog
        mode: "missingDeps"
    }

    ConfirmationDialog {
        id: unloadCascadeDialog
        mode: "unloadCascade"
        onContinueClicked: (name) => backend.confirmUnloadCascade(name)
        onCancelClicked: (name) => backend.cancelPendingAction(name)
    }

    ConfirmationDialog {
        id: uninstallCascadeDialog
        mode: "uninstallCascade"
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
        onContinueClicked: (name) => backend.confirmUninstallCascade(name)
        onCancelClicked: (name) => backend.cancelPendingAction(name)
    }

    ConfirmationDialog {
        id: installConfirmDialog
        mode: "installConfirm"
        onContinueClicked: backend.confirmInstall()
        onCancelClicked: backend.cancelInstall()
    }

    // App-Manager "Add Application" dialog.
    AddApplicationDialog {
        id: addApplicationDialog
        requiredPackagesModel: backend.requiredPackagesModel
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

    Rectangle {
        id: sidebarTooltip
        visible: opacity > 0
        opacity: tooltipTimer.showIt ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        x: 68
        y: backend.sidebarTooltipY - height / 2
        width: tooltipLabel.implicitWidth + 16
        height: tooltipLabel.implicitHeight + 8
        radius: 4
        color: Theme.palette.surface

        LogosText {
            id: tooltipLabel
            anchors.centerIn: parent
            text: backend.sidebarTooltipText
            font.pixelSize: 12
            color: Theme.palette.textSecondary
        }

        Timer {
            id: tooltipTimer
            property bool showIt: false
            interval: 500
            onTriggered: showIt = true
        }
    }

    Connections {
        target: backend
        function onSidebarTooltipChanged() {
            if (backend.sidebarTooltipText !== "") {
                tooltipTimer.showIt = false
                tooltipTimer.restart()
            } else {
                tooltipTimer.stop()
                tooltipTimer.showIt = false
            }
        }
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
                                                       installedDependents, loadedDependents) {
            upgradeCascadeDialog.openWithUpgrade(name, releaseTag, mode,
                                                 installedDependents, loadedDependents);
        }

        function onInstallConfirmationRequested(metadata) {
            installConfirmDialog.openWithMetadata(metadata);
        }

        function onLaunchAppRequested(name) {
            backend.onAppLauncherClicked(name);
        }

        function onAddApplicationRequested(metadata, requiredPackages) {
            if (addApplicationDialog.visible) {
                addApplicationDialog.metadata = metadata;
                addApplicationDialog.installStage = metadata.installStage || InstallStage.None;
            } else {
                addApplicationDialog.openWith(metadata);
            }
        }

        function onCatalogInstallStageChanged(name, stage) {
            if (!addApplicationDialog.visible) return;
            if (addApplicationDialog.metadata.name !== name) return;
            addApplicationDialog.installStage = stage;
        }
        function onCatalogInstallFinished(name) {
            if (addApplicationDialog.visible
                && addApplicationDialog.metadata.name === name) {
                addApplicationDialog.markInstallComplete();
            }
        }
    }
}
