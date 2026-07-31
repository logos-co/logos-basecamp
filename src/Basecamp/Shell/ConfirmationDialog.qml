import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Icons
import Logos.Theme

// Reusable dialog for dependency-aware confirmation / informational prompts.
//
// Five display variants, selected via `mode`:
//  - "missingDeps"    — informational; user tried to load a plugin whose
//                       dependencies aren't installed. Primary action is a
//                       "Continue" that closes the dialog (the primary
//                       button is relabelled to "Open Package Manager" when
//                       packageManagerNavigation is wired).
//  - "unloadCascade"  — confirmation; unloading this module would leave
//                       other loaded modules stranded. Continue cascades
//                       the unload via the backend; Cancel aborts.
//  - "uninstallCascade" — confirmation; uninstalling this module will
//                       break installed dependents and unload currently
//                       loaded dependents. Renders TWO labelled lists:
//                         * items        — installed dependents (full impact)
//                         * loadedItems  — subset to be torn down now
//                       When only `items` is populated and `loadedItems` is
//                       empty, we still show the installed list so the user
//                       sees what will break next time those are loaded.
//  - "upgradeCascade" — confirmation; upgrading/downgrading/reinstalling
//                       this module. Same two-list shape as uninstallCascade
//                       (the package_manager performs an uninstall step
//                       first, with the same dependent-impact set), but
//                       title and body lead with the new version + the
//                       UpgradeMode (Upgrade / Downgrade / Reinstall) so
//                       the user knows the operation isn't a bare uninstall.
//                       Confirm/Cancel still flow through `continueClicked`
//                       / `cancelClicked`; the receiver disambiguates from
//                       its own pending state (UpgradeCascade vs
//                       UninstallCascade).
//  - "installGate"    — confirmation before a fresh install routed through the
//                       package_manager requestInstall gate. Every install the
//                       app performs comes through here: package_manager_ui
//                       initiates them all, whether the source is a catalog
//                       download or a local .lgx the user picked. Leads with the
//                       package + target version and lists the resolved
//                       transitive `depChanges`. No dependent-impact lists (a
//                       fresh install unloads nothing). Confirm/Cancel flow
//                       through continueClicked / cancelClicked →
//                       confirmInstallGate / cancelInstallGate.
//
//                       Note: for a local .lgx the initiator has no catalog to
//                       resolve against, so `depChanges` arrives empty and the
//                       dialog is name + version only. Surfacing the inspected
//                       manifest here needs the metadata carried through the
//                       gate payload — see requestInstall's depChanges, which
//                       establishes the "opaque display data" precedent.
//
// The dialog is controlled by calling `openWith(mode, name, items)` for the
// one-list modes, `openWithTwoLists(mode, name, items, loadedItems)` for
// uninstallCascade, `openWithUpgrade(name, version, upgradeMode, installedDeps, loadedDeps, depChanges)`
// for upgradeCascade, or `openWithInstallGate(name, version, depChanges)` for
// the install gate. Backend wiring listens for continueClicked/cancelClicked
// and calls the appropriate slot with `name`.
Dialog {
    id: root

    // "missingDeps" | "unloadCascade" | "uninstallCascade" | "upgradeCascade" | "installGate"
    property string mode: "missingDeps"
    property string moduleName: ""
    property var items: []
    // Only used in uninstall/upgrade cascade modes — the subset of `items`
    // that is currently loaded and will be torn down as part of the
    // cascade.
    property var loadedItems: []
    // Only used in uninstallCascade for the multi-package variant
    property var targets: []
    // Only used in upgradeCascade mode. `upgradeTargetVersion` is the
    // pinned target (e.g. "1.0.0") supplied by the caller's requestUpgrade
    // call; `upgradeModeKind` mirrors PackageTypes/UpgradeMode —
    //   0 = Upgrade, 1 = Downgrade, 2 = Sidegrade (Reinstall).
    property string upgradeTargetVersion: ""
    property int upgradeModeKind: 0

    // Transitive dependency changes for the upgradeCascade + installGate modes.
    // Each entry: { name, action: "install"|"upgrade"|"downgrade",
    //               fromVersion, toVersion, repository }. Resolved by the
    //               initiator (package_manager_ui) and passed through the
    //               module's beforeUpgrade / beforeInstall gate so this single
    //               dialog lists exactly what else will change. Empty = nothing
    //               else needs to change.
    property var depChanges: []

    property var displayNameLookup: function(name) { return name; }

    // Internal: tracks whether a button explicitly handled the close
    // so the onClosed handler doesn't double-fire cancelClicked. Set
    // true in continue/cancel onClicked before we call close().
    property bool _explicitClose: false

    signal continueClicked(string name)
    signal cancelClicked(string name)
    signal continueClickedMulti(var names)
    signal cancelClickedMulti(var names)

    modal: true
    anchors.centerIn: parent
    width: 560
    padding: Theme.spacing.large
    closePolicy: Popup.CloseOnEscape

    // API for parent components — simpler than setting props + open() each time.
    function openWith(mode_, name_, items_) {
        root.mode = mode_;
        root.moduleName = name_ || "";
        root.items = items_ || [];
        root.loadedItems = [];
        root.targets = [];
        root._explicitClose = false;
        open();
    }

    // Two-list variant for uninstallCascade — installedDeps is the full set
    // that depends on this module (renders as "Will stop working"), loadedDeps
    // is the subset currently running (renders as "Will be unloaded now").
    function openWithTwoLists(mode_, name_, installedDeps_, loadedDeps_) {
        root.mode = mode_;
        root.moduleName = name_ || "";
        root.items = installedDeps_ || [];
        root.loadedItems = loadedDeps_ || [];
        root.targets = [];
        root._explicitClose = false;
        open();
    }

    // Multi-target variant for uninstallCascade. Continue / Cancel emit the
    // *Multi signals so callers route to confirmMultiUninstall / cancelMultiUninstall.
    function openWithMultiTargets(targets_, installedDeps_, loadedDeps_) {
        root.mode = "uninstallCascade";
        root.moduleName = "";
        root.items = installedDeps_ || [];
        root.loadedItems = loadedDeps_ || [];
        root.targets = targets_ || [];
        root._explicitClose = false;
        open();
    }

    // Upgrade/Downgrade/Reinstall variant. Reuses the two-list dependent
    // impact shape (the upgrade flow does an uninstall step first, with
    // the same cascade semantics) but the title + body line lead with
    // the target version and the UpgradeMode so the user sees the full
    // operation, not just "Uninstall and Unload Dependents?".
    function openWithUpgrade(name_, version_, upgradeMode_, installedDeps_, loadedDeps_, depChanges_) {
        root.mode = "upgradeCascade";
        root.moduleName = name_ || "";
        root.upgradeTargetVersion = version_ || "";
        root.upgradeModeKind = upgradeMode_ | 0;
        root.items = installedDeps_ || [];
        root.loadedItems = loadedDeps_ || [];
        root.depChanges = depChanges_ || [];
        root.targets = [];
        root._explicitClose = false;
        open();
    }

    // Fresh catalog-install variant. Unlike upgradeCascade there is no
    // dependent-impact set (nothing is uninstalled/unloaded); the dialog
    // simply confirms the install and lists the transitive `depChanges`.
    // Continue / Cancel still flow through continueClicked / cancelClicked;
    // the backend routes those to confirmInstallGate / cancelInstallGate.
    function openWithInstallGate(name_, version_, depChanges_) {
        root.mode = "installGate";
        root.moduleName = name_ || "";
        root.upgradeTargetVersion = version_ || "";
        root.items = [];
        root.loadedItems = [];
        root.depChanges = depChanges_ || [];
        root.targets = [];
        root._explicitClose = false;
        open();
    }

    background: Rectangle {
        color: Theme.palette.surfaceRaised
        border.color: Theme.palette.border
        border.width: 1
        radius: 8
    }

    contentItem: ColumnLayout {
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            LogosIcon {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                source: LogosIcons.warning
                color: Theme.palette.error
                brightness: 1.0
            }

            LogosText {
                Layout.fillWidth: true
                text: {
                    if (root.mode === "missingDeps")
                        return "Missing Dependencies";
                    if (root.mode === "unloadCascade")
                        return "Unload Dependent Modules?";
                    if (root.mode === "uninstallCascade") {
                        if (root.targets.length > 1)
                            return "Uninstall " + root.targets.length + " packages?";
                        return "Uninstall and Unload Dependents?";
                    }
                    if (root.mode === "upgradeCascade") {
                        if (root.upgradeModeKind === 1) return "Downgrade Package?";
                        if (root.upgradeModeKind === 2) return "Reinstall Package?";
                        return "Upgrade Package?";
                    }
                    if (root.mode === "installGate")
                        return "Install Package?";
                    return "";
                }
                font.pixelSize: Theme.typography.panelTitleText
                font.weight: Theme.typography.weightBold
                color: Theme.palette.text
                wrapMode: Text.Wrap
            }
        }

        LogosText {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.palette.textSecondary
            readonly property string _label: root.displayNameLookup(root.moduleName) || root.moduleName
            text: {
                if (root.mode === "missingDeps")
                    return "'" + _label + "' cannot be loaded because the "
                         + "following modules are not installed:";
                if (root.mode === "unloadCascade")
                    return "The following modules are currently loaded and depend on '"
                         + _label + "'. Unloading will terminate them:";
                if (root.mode === "uninstallCascade") {
                    if (root.targets.length > 1) {
                        if (root.items.length === 0 && root.loadedItems.length === 0)
                            return "The following packages will be removed from disk:";
                        return "The following packages will be removed from disk "
                             + "and affect the listed dependents:";
                    }
                    // Collapse to a simple destructive-action confirmation
                    // when neither list is populated.
                    if (root.items.length === 0 && root.loadedItems.length === 0)
                        return "Uninstall '" + _label + "'? This will "
                             + "remove the package files from disk.";
                    return "Uninstalling '" + _label + "' will remove the "
                         + "package files from disk and affect the following:";
                }
                if (root.mode === "upgradeCascade") {
                    // Lead with the operation in plain English so the
                    // user sees this isn't a bare uninstall — the
                    // package_manager removes the current version as the
                    // first phase of the swap, which is why this dialog
                    // fires at all. Target version goes here (not in the
                    // title) so long version strings don't truncate.
                    var verb = "Upgrade";
                    if (root.upgradeModeKind === 1) verb = "Downgrade";
                    else if (root.upgradeModeKind === 2) verb = "Reinstall";
                    var verPhrase = root.upgradeTargetVersion.length > 0
                                    ? " to v" + root.upgradeTargetVersion : "";
                    var head = verb + " '" + _label + "'" + verPhrase
                             + ". The current version will be removed first, "
                             + "then the new one downloaded and installed.";
                    // We only surface the *loaded* dependents here — the
                    // installed-but-not-running set picks up the new
                    // version on their next load and isn't "affected" in
                    // any user-visible way, so listing them implies
                    // alarm that doesn't exist (this was the
                    // pre-existing copy bug). If nothing is currently
                    // running on top of the module, the head sentence
                    // stands on its own.
                    if (root.loadedItems.length === 0)
                        return head;
                    return head + " The modules below are running on top of it and "
                                + "will be unloaded for the swap — they keep working "
                                + "with the new version once it lands.";
                }
                if (root.mode === "installGate") {
                    var ivPhrase = root.upgradeTargetVersion.length > 0
                                   ? " v" + root.upgradeTargetVersion : "";
                    var iHead = "Install '" + _label + "'" + ivPhrase + ".";
                    // The dep-change list below spells out the transitive set.
                    // When it's empty, say so plainly so a bare install still
                    // reads as a deliberate, complete confirmation.
                    if ((root.depChanges || []).length === 0)
                        return iHead + " No other packages need to change.";
                    return iHead + " Installing it also applies the dependency "
                                 + "changes listed below:";
                }
                return "";
            }
        }

        // Single-list block — used by missingDeps and unloadCascade.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(150, Math.max(30, root.items.length * 24))
            color: "#1e1e1e"
            radius: 4
            border.color: "#3d3d3d"
            border.width: 1
            visible: root.mode !== "uninstallCascade"
                     && root.mode !== "upgradeCascade"
                     && root.items.length > 0

            LogosListView {
                id: itemList
                anchors.fill: parent
                anchors.margins: 8
                model: root.items
                clip: true
                delegate: LogosText {
                    text: "• " + (root.displayNameLookup(modelData) || modelData)
                    color: "#e0e0e0"
                    font.pixelSize: 13
                }
            }
        }

        // Targets list — only rendered for the multi-package variant of
        // uninstallCascade. Lists the packages the user explicitly selected
        // for uninstallation. The two-list block below renders the *dependents*
        // affected by removing them.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.mode === "uninstallCascade" && root.targets.length > 1

            LogosText {
                Layout.fillWidth: true
                text: "Packages to uninstall:"
                color: "#c0c0c0"
                font.pixelSize: 13
                font.weight: Theme.typography.weightBold
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(150, Math.max(30, root.targets.length * 24))
                color: "#1e1e1e"
                radius: 4
                border.color: "#3d3d3d"
                border.width: 1

                LogosListView {
                    anchors.fill: parent
                    anchors.margins: 8
                    model: root.targets
                    clip: true
                    delegate: LogosText {
                        text: "• " + (root.displayNameLookup(modelData) || modelData)
                        color: "#e0e0e0"
                        font.pixelSize: 13
                    }
                }
            }
        }

        // Two-list block for uninstallCascade. Rendered as a single Column so
        // the section headers and lists share the dialog's vertical spacing.
        // Each sub-list is only shown when it has entries — a pure loaded-only
        // or installed-only case still renders cleanly as one labelled list.
        //
        // upgradeCascade reuses the loaded-list half only: installed-not-
        // running dependents pick up the new version on their next load and
        // aren't user-visibly affected, so listing them under "Will stop
        // working" would be a lie. The body sentence above already covers
        // the "running dependents get a brief unload" story, the list below
        // just names them.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: (root.mode === "uninstallCascade" && (root.items.length > 0 || root.loadedItems.length > 0))
                  || (root.mode === "upgradeCascade"   && root.loadedItems.length > 0)

            // Installed dependents (full impact — will break on next load).
            // Hidden for upgradeCascade: the new version replaces the old
            // before they next load, so they're not breaking.
            LogosText {
                Layout.fillWidth: true
                text: "Will stop working (installed but not running):"
                color: "#c0c0c0"
                font.pixelSize: 13
                font.weight: Theme.typography.weightBold
                wrapMode: Text.Wrap
                visible: root.mode === "uninstallCascade" && root.items.length > 0
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(120, Math.max(30, root.items.length * 24))
                color: "#1e1e1e"
                radius: 4
                border.color: "#3d3d3d"
                border.width: 1
                visible: root.mode === "uninstallCascade" && root.items.length > 0

                LogosListView {
                    anchors.fill: parent
                    anchors.margins: 8
                    model: root.items
                    clip: true
                    delegate: LogosText {
                        text: "• " + (root.displayNameLookup(modelData) || modelData)
                        color: "#e0e0e0"
                        font.pixelSize: 13
                    }
                }
            }

            // Loaded dependents — wording differs by mode. uninstallCascade
            // unloads them permanently (the module they hang off is going
            // away); upgradeCascade unloads them only for the swap (they
            // resume against the new version once it's installed).
            LogosText {
                Layout.fillWidth: true
                Layout.topMargin: 4
                text: root.mode === "upgradeCascade"
                      ? "Currently running (will be temporarily unloaded):"
                      : "Will be unloaded now:"
                color: "#c0c0c0"
                font.pixelSize: 13
                font.weight: Theme.typography.weightBold
                wrapMode: Text.Wrap
                visible: root.loadedItems.length > 0
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(120, Math.max(30, root.loadedItems.length * 24))
                color: "#1e1e1e"
                radius: 4
                border.color: "#3d3d3d"
                border.width: 1
                visible: root.loadedItems.length > 0

                LogosListView {
                    anchors.fill: parent
                    anchors.margins: 8
                    model: root.loadedItems
                    clip: true
                    delegate: LogosText {
                        text: "• " + (root.displayNameLookup(modelData) || modelData)
                        color: "#e0e0e0"
                        font.pixelSize: 13
                    }
                }
            }
        }

        // Transitive dependency-change list — shared by upgradeCascade and
        // installGate. Renders each change as [action chip] name (repo) vA → vB,
        // mirroring the package_manager_ui per-row confirm so the single
        // basecamp dialog surfaces exactly what else the operation installs /
        // upgrades / downgrades. Hidden when the initiator resolved no changes.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            visible: (root.mode === "upgradeCascade" || root.mode === "installGate")
                     && (root.depChanges || []).length > 0

            LogosText {
                Layout.fillWidth: true
                Layout.topMargin: 4
                text: "Dependency changes:"
                color: "#c0c0c0"
                font.pixelSize: 13
                font.weight: Theme.typography.weightBold
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                color: "#1e1e1e"
                radius: 4
                border.color: "#3d3d3d"
                border.width: 1
                implicitHeight: depChangeList.implicitHeight + 16

                LogosListView {
                    id: depChangeList
                    anchors.fill: parent
                    anchors.margins: 8
                    model: root.depChanges
                    spacing: 4
                    clip: true
                    interactive: contentHeight > height
                    implicitHeight: Math.min(160, Math.max(contentHeight, 0))

                    delegate: RowLayout {
                        width: ListView.view ? ListView.view.width : 0
                        spacing: 8

                        // Action chip — colour-keyed like the PMU row pills.
                        Rectangle {
                            Layout.preferredWidth: 76
                            Layout.preferredHeight: 20
                            radius: 10
                            color: {
                                switch (modelData.action) {
                                case "install":   return Theme.colors.getColor(Theme.palette.success, 0.18);
                                case "upgrade":   return Theme.colors.getColor(Theme.palette.info,    0.18);
                                case "downgrade": return Theme.colors.getColor(Theme.palette.warning, 0.18);
                                default:          return Theme.colors.getColor(Theme.palette.info,    0.18);
                                }
                            }
                            LogosText {
                                anchors.centerIn: parent
                                text: {
                                    var a = modelData.action || "";
                                    return a ? a.charAt(0).toUpperCase() + a.slice(1) : "";
                                }
                                color: {
                                    switch (modelData.action) {
                                    case "install":   return Theme.palette.success;
                                    case "upgrade":   return Theme.palette.info;
                                    case "downgrade": return Theme.palette.warning;
                                    default:          return Theme.palette.text;
                                    }
                                }
                                font.pixelSize: 12
                                font.weight: Theme.typography.weightMedium
                            }
                        }

                        // name (repo)  vFrom → vTo
                        LogosText {
                            Layout.fillWidth: true
                            text: {
                                var name = modelData.name || "";
                                var repo = modelData.repository || "";
                                var to   = modelData.toVersion ? "v" + modelData.toVersion : "";
                                var from = modelData.fromVersion ? "v" + modelData.fromVersion : "";
                                var ver  = (from && to && from !== to) ? (from + " → " + to)
                                                                       : (to ? to : "");
                                var head = repo ? (name + " (" + repo + ")") : name;
                                return ver ? (head + "  " + ver) : head;
                            }
                            color: "#e0e0e0"
                            font.pixelSize: 13
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 8
            spacing: 8

            Item { Layout.fillWidth: true }

            // Cancel button — hidden in informational mode since there's
            // only one button to press there.
            //
            // Both buttons carry a mode-derived objectName so UI automation
            // can target them exactly. Text-based clicking is ambiguous here:
            // labels like "Uninstall" also appear on the module-row buttons,
            // and the inspector's text search doesn't distinguish open from
            // closed dialogs.
            LogosButton {
                objectName: "confirmationDialog." + root.mode + ".cancel"
                text: "Cancel"
                visible: root.mode !== "missingDeps"
                onClicked: {
                    root._explicitClose = true;
                    if (root.targets.length > 0)
                        root.cancelClickedMulti(root.targets);
                    else
                        root.cancelClicked(root.moduleName);
                    root.close();
                }
            }

            LogosButton {
                objectName: "confirmationDialog." + root.mode + ".confirm"
                text: {
                    if (root.mode === "missingDeps") return "OK";
                    if (root.mode === "unloadCascade") return "Unload All";
                    if (root.mode === "uninstallCascade") {
                        if (root.targets.length > 1)
                            return "Uninstall " + root.targets.length;
                        return "Uninstall";
                    }
                    if (root.mode === "upgradeCascade") {
                        if (root.upgradeModeKind === 1) return "Downgrade";
                        if (root.upgradeModeKind === 2) return "Reinstall";
                        return "Upgrade";
                    }
                    if (root.mode === "installGate") return "Install";
                    return "OK";
                }
                variant: LogosButton.Variant.Primary
                onClicked: {
                    root._explicitClose = true;
                    if (root.targets.length > 0)
                        root.continueClickedMulti(root.targets);
                    else
                        root.continueClicked(root.moduleName);
                    root.close();
                }
            }
        }
    }

    onClosed: {
        // Auto-cancel on Escape and other Dialog-managed dismissals for
        // cascade modes so the pending state in the backend gets cleared.
        // closePolicy is Popup.CloseOnEscape only — outside-click does NOT
        // dismiss these dialogs (they're destructive and require an
        // explicit button). A button click sets _explicitClose before
        // calling close(), so this onClosed handler only fires on
        // dismissals that went through the Dialog's own close path
        // (Escape today; any future policy additions as well).
        if (root._explicitClose) {
            root._explicitClose = false;
            return;
        }
        if (root.mode === "unloadCascade" || root.mode === "uninstallCascade"
            || root.mode === "upgradeCascade" || root.mode === "installGate") {
            if (root.targets.length > 0)
                root.cancelClickedMulti(root.targets);
            else
                root.cancelClicked(root.moduleName);
        }
    }
}
