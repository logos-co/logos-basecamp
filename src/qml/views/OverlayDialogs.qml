import QtQuick
import QtQuick.Controls
import controls

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
                                  || installConfirmDialog.visible

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

    ConfirmationDialog {
        id: installConfirmDialog
        mode: "installConfirm"
        onContinueClicked: backend.confirmInstall()
        onCancelClicked: backend.cancelInstall()
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

        function onInstallConfirmationRequested(metadata) {
            installConfirmDialog.openWithMetadata(metadata);
        }
    }
}
