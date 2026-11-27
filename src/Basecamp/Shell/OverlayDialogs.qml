import QtQuick
import QtQuick.Controls
import Logos.Theme
import Logos.Controls
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
                                  || loadFailedDialog.visible
                                  || intentChooserDialog.visible
                                  || intentInstallDialog.visible
    property string sidebarTooltipText: ""
    property real   sidebarTooltipY:    0

    signal overlayActiveChanged(bool active)

    onAnyDialogOpenChanged: {
        root.overlayActiveChanged(anyDialogOpen)
        backend.setOverlayActive(anyDialogOpen)
    }

    QtObject {
        id: _dialogDeps
        property var displayNameLookup: function(name) { return backend.displayNameFor(name); }
        readonly property string appsApp: backend.sectionApps
                                          ? backend.sectionApps[ShellSection.AppManager] || "" : ""
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

    // The one uninstall confirmation, for one package or several.
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
            backend.cancelPendingAction(plan.targetName)
        }
    }

    // Distinct dialog instance for upgrade/downgrade/reinstall cascades so
    // the title + body can lead with the target version + UpgradeMode
    // instead of "Uninstall and Unload Dependents?" (the previous
    // shared-with-uninstall dialog confused users on downgrades). Confirm/
    // Cancel route through the same backend slots — the shell-side work is
    // identical (unload, then answer); only what PMU does next differs.
    ConfirmationDialog {
        id: upgradeCascadeDialog
        objectName: "confirmationDialog.upgradeCascade"
        mode: "upgradeCascade"
        displayNameLookup: _dialogDeps.displayNameLookup
        appsApp: _dialogDeps.appsApp
        onContinueClicked: (name) => backend.confirmUninstallCascade(name, upgradeCascadeDialog.selectedOptionalNames(), upgradeCascadeDialog.optionalPickedVersions)
        onTrustAndContinueClicked: (name) => backend.confirmUninstallCascade(name, upgradeCascadeDialog.selectedOptionalNames(), upgradeCascadeDialog.optionalPickedVersions, true)
        onCancelClicked: (name) => backend.cancelPendingAction(name)
        onOptionalPreviewRequested: (name, selection, pins) => backend.refreshOptionalPreview(name, selection, pins)
    }

    // Install gate raised by App Manager or package_manager_ui as
    // `confirm_install` — the only install confirmation in the app, covering
    // both catalog downloads and local .lgx picks. Confirm/cancel answer the
    // intent so the app installs (or aborts). Lists the resolved transitive dep
    // changes.
    ConfirmationDialog {
        id: installGateDialog
        objectName: "confirmationDialog.installGate"
        mode: "installGate"
        displayNameLookup: _dialogDeps.displayNameLookup
        appsApp: _dialogDeps.appsApp
        onContinueClicked: (name) => backend.confirmInstallGate(name, installGateDialog.selectedOptionalNames(), installGateDialog.optionalPickedVersions)
        onTrustAndContinueClicked: (name) => backend.confirmInstallGate(name, installGateDialog.selectedOptionalNames(), installGateDialog.optionalPickedVersions, true)
        onOptionalPreviewRequested: (name, selection, pins) => backend.refreshOptionalPreview(name, selection, pins)
        onCancelClicked: (name) => backend.cancelInstallGate(name)
    }

    // NOTHING installed provides the requested capability, but the catalog has a
    // package that would. IntentBroker gates this on Resolution::None — the
    // two-or-more case raises the chooser instead.
    IntentInstallDialog {
        id: intentInstallDialog
        displayNameLookup: _dialogDeps.displayNameLookup
        onInstallRequested: function(providerName) {
            backend.beginIntentInstall(providerName);
        }
    }

    IntentChooserDialog {
        id: intentChooserDialog
        displayNameLookup: _dialogDeps.displayNameLookup
        detailsLookup: function(name) { return backend.providerDetailsFor(name); }
        installableLookup: function(intent) { return backend.installableProvidersFor(intent); }
        onProviderChosen: function(dispatchId, providerName) {
            backend.resolveIntentChooser(dispatchId, providerName);
        }
        onChoiceCancelled: function(dispatchId) {
            backend.cancelIntentChooser(dispatchId);
        }
        onChoiceClosed: function(dispatchId) {
            backend.intentChooserClosed(dispatchId);
        }
        // Ordering is the dialog's: it cancels first, so by the time this
        // arrives the request has been answered and the install resumes nothing.
        onInstallRequested: function(intent, providerName) {
            backend.offerInstallFor(intent, providerName);
        }

    }

    ConfirmationDialog {
        id: installErrorDialog
        objectName: "confirmationDialog.installError"
        mode: "installError"
        displayNameLookup: _dialogDeps.displayNameLookup
    }

    // A UI app failed to load (QML compile error, missing view, ui-host
    // spawn/ready failure, ...). Informational only — PluginLoader has
    // already cleaned up and cleared the sidebar loading state.
    ConfirmationDialog {
        id: loadFailedDialog
        objectName: "confirmationDialog.loadFailed"
        mode: "loadFailed"
        displayNameLookup: _dialogDeps.displayNameLookup
    }

    LogosToast {
        id: linkFailedToast
        objectName: "linkFailedToast"
        parent: root
        anchors.horizontalCenter: root.horizontalCenter
        anchors.bottom: root.bottom
        anchors.bottomMargin: Theme.spacing.large
        width: Math.min(root.width - Theme.spacing.large * 2, 480)
        severity: LogosNotice.Error
        title: qsTr("Link could not be opened")
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

        function onLinkFailed(reason) {
            linkFailedToast.show(undefined, reason);
        }

        function onIntentInstallOffered(intent, candidates, details) {
            intentInstallDialog.openWith(intent, candidates, details);
        }

        function onIntentChooserRequested(dispatchId, intent, requesterName, providers) {
            intentChooserDialog.openWith({
                dispatchId: dispatchId,
                intent: intent,
                requesterName: requesterName,
                providers: providers
            });
        }

        // The broker ended the request some other way (the requester died, the
        // backstop fired). Close only if this is still the one on screen.
        function onIntentChooserDismissed(dispatchId) {
            intentChooserDialog.closeFor(dispatchId);
        }

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
                                                       depChanges, requesterName,
                                                       requesterBundled, requestId) {
            upgradeCascadeDialog.openWithUpgrade(name, releaseTag, mode,
                                                 installedDependents, loadedDependents,
                                                 depChanges, requesterName, requesterBundled,
                                                 requestId);
        }

        // Install gate (package_manager_ui-initiated). releaseTag is the
        // target version; depChanges is the resolved transitive set.
        // Its request ended unanswered: nothing would hear the answer.
        function onConfirmationWithdrawn(requestId) {
            for (const dialog of [installGateDialog, upgradeCascadeDialog, uninstallDialog])
                dialog.withdraw(requestId);
        }

        function onInstallGateConfirmationRequested(name, releaseTag, depChanges,
                                                    requesterName, requesterBundled,
                                                    depChangesResolved, requestId) {
            installGateDialog.openWithInstallGate(name, releaseTag, depChanges,
                                                  requesterName, requesterBundled,
                                                  depChangesResolved, requestId);
        }

        function onInstallTrustUpdated(name, trust) {
            if (installGateDialog.visible && installGateDialog.moduleName === name)
                installGateDialog.trust = trust;
            if (upgradeCascadeDialog.visible && upgradeCascadeDialog.moduleName === name)
                upgradeCascadeDialog.trust = trust;
        }

        function onOptionalGatePreviewUpdated(name, changes, pending) {
            const dialog = installGateDialog.visible && installGateDialog.moduleName === name
                ? installGateDialog : upgradeCascadeDialog;
            if (!dialog.visible || dialog.moduleName !== name) return;
            dialog.resolutionPending = pending;
            if (!pending) dialog.depChanges = changes;
        }

        function onInstallFailureNoticeRequested(name, errorMessage) {
            installErrorDialog.openWithInstallError(name, errorMessage);
        }

        function onLoadFailureNoticeRequested(name, errorMessage) {
            loadFailedDialog.openWithLoadError(name, errorMessage);
        }

        function onStartupNoticeRequested(title, name, message) {
            loadFailedDialog.openWithLoadError(name, message, title);
        }
    }
}
