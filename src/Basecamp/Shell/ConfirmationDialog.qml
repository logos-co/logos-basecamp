import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Icons
import Logos.Theme

// Reusable dialog for dependency-aware confirmation / informational prompts.
//
// Display variants, selected via `mode`:
//  - "missingDeps"    — informational; user tried to load a plugin whose
//                       dependencies won't let it load. The only action is an
//                       "OK" that closes the dialog — Cancel is hidden, since
//                       nothing was started and there is nothing to abort.
//
//                       THREE different things bring a user here, and the copy
//                       must not conflate them: a dependency that is not
//                       installed; one that IS installed at a version outside
//                       the range the manifest declared; and one that is
//                       installed under the right name but published by a
//                       DIFFERENT SIGNER, which is somebody else's package and
//                       cannot be fixed by installing or by changing version.
//                       `blockSummary` ("absent" | "mismatch" | "signer" |
//                       "mixed") picks the sentence, and each row carries its
//                       own `detail` clause naming the constraint and what was
//                       found — "requires ^2.0.0, found 1.0.0", or "published
//                       by a different signer; requires <did>, found <did>".
//                       Without that pair the dialog told a user with a version
//                       conflict that the module was "not installed", which is
//                       both false and unactionable.
//  - "unloadCascade"  — confirmation; unloading this module would leave
//                       other loaded modules stranded. Continue cascades
//                       the unload via the backend; Cancel aborts.
//  - "upgradeCascade" — confirmation; upgrading/downgrading/reinstalling
//                       this module. The package_manager performs an
//                       uninstall step first, so currently-running dependents
//                       are unloaded for the swap; `loadedItems` names them.
//                       Installed-but-not-running dependents are NOT listed:
//                       they pick up the new version on their next load and
//                       aren't user-visibly affected. Title and body lead
//                       with the new version + the UpgradeMode (Upgrade /
//                       Downgrade / Reinstall) so the user knows the
//                       operation isn't a bare uninstall. Confirm/Cancel flow
//                       through `continueClicked` / `cancelClicked`.
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
//                       dialog is name + version only.
//  - "installError"   — informational; an install failed. Shows the package
//                       (or picked file) name + the reported error, OK only.
//
// Uninstall confirmation is NOT here — it lives in UninstallDialog.qml, which
// renders a resolved plan (what goes, what stays and why, what breaks) rather
// than a pair of dependent lists.
//
// The dialog is controlled by calling `openWith(mode, name, items,
// blockSummary)` for the one-list modes, `openWithUpgrade(name, version, upgradeMode, installedDeps, loadedDeps, depChanges)`
// for upgradeCascade, or `openWithInstallGate(name, version, depChanges)` for
// the install gate. Backend wiring listens for continueClicked/cancelClicked
// and calls the appropriate slot with `name`.
Dialog {
    id: root

    // "missingDeps" | "unloadCascade" | "upgradeCascade" | "installGate" | "installError"
    property string mode: "missingDeps"
    property string moduleName: ""
    // For "missingDeps" each entry is a map from
    // logos::dependencyBlockerToMap — {name, kind, requiredVersion,
    // installedVersion, requiredSigner, observedSigner, detail}. For the other
    // one-list modes it is a plain module name. `_itemName` / `_itemDetail`
    // read either shape.
    property var items: []
    // Only used in missingDeps mode:
    // "" | "absent" | "mismatch" | "signer" | "mixed".
    // Computed host-side (logos::summariseDependencyBlockers) so one set of
    // blockers yields one sentence everywhere it is described.
    property string blockSummary: ""
    // Only used in upgradeCascade mode — the dependents currently loaded,
    // which get torn down for the version swap.
    property var loadedItems: []
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

    property string errorMessage: ""

    property var displayNameLookup: function(name) { return name; }

    // Internal: tracks whether a button explicitly handled the close
    // so the onClosed handler doesn't double-fire cancelClicked. Set
    // true in continue/cancel onClicked before we call close().
    property bool _explicitClose: false

    signal continueClicked(string name)
    signal cancelClicked(string name)

    modal: true
    anchors.centerIn: parent
    width: 560
    padding: Theme.spacing.large
    closePolicy: Popup.CloseOnEscape

    // API for parent components — simpler than setting props + open() each time.
    function openWith(mode_, name_, items_, blockSummary_) {
        root.mode = mode_;
        root.moduleName = name_ || "";
        root.items = items_ || [];
        root.blockSummary = blockSummary_ || "";
        root.loadedItems = [];
        root._explicitClose = false;
        open();
    }

    // An `items` entry is either a blocker map or a bare module name; these
    // two read either without a stringification that would quietly turn a map
    // into an empty label.
    function _itemName(entry) {
        if (entry === undefined || entry === null) return "";
        if (typeof entry === "string") return entry;
        return entry.name !== undefined ? entry.name : "";
    }

    function _itemDetail(entry) {
        if (entry === undefined || entry === null || typeof entry === "string")
            return "";
        return entry.detail !== undefined ? entry.detail : "";
    }

    // Upgrade/Downgrade/Reinstall variant. The upgrade flow does an uninstall
    // step first, so it carries the same loaded-dependent set as an uninstall
    // would — but the title + body lead with the target version and the
    // UpgradeMode so the user sees the full operation, not just
    // "Uninstall and Unload Dependents?".
    function openWithUpgrade(name_, version_, upgradeMode_, installedDeps_, loadedDeps_, depChanges_) {
        root.mode = "upgradeCascade";
        root.moduleName = name_ || "";
        root.upgradeTargetVersion = version_ || "";
        root.upgradeModeKind = upgradeMode_ | 0;
        root.items = installedDeps_ || [];
        root.loadedItems = loadedDeps_ || [];
        root.depChanges = depChanges_ || [];
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
        root._explicitClose = false;
        open();
    }

    function openWithInstallError(name_, errorMessage_) {
        root.mode = "installError";
        root.moduleName = name_ || "";
        root.errorMessage = errorMessage_ || "";
        root.items = [];
        root.loadedItems = [];
        root.depChanges = [];
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
                    if (root.mode === "missingDeps") {
                        // Named, not defaulted. "Missing Dependencies" is the
                        // fallback ONLY for the shapes where something really
                        // is missing ("absent", "mixed"); a shape that means
                        // everything is present would inherit a title that
                        // contradicts its own body text.
                        if (root.blockSummary === "signer")   return "Unexpected Publisher";
                        if (root.blockSummary === "mismatch") return "Incompatible Dependencies";
                        return "Missing Dependencies";
                    }
                    if (root.mode === "unloadCascade")
                        return "Unload Dependent Modules?";
                    if (root.mode === "upgradeCascade") {
                        if (root.upgradeModeKind === 1) return "Downgrade Package?";
                        if (root.upgradeModeKind === 2) return "Reinstall Package?";
                        return "Upgrade Package?";
                    }
                    if (root.mode === "installGate")
                        return "Install Package?";
                    if (root.mode === "installError")
                        return "Install Failed";
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
                if (root.mode === "missingDeps") {
                    // Four different facts, four different sentences. Saying
                    // "not installed" about a module that is installed at the
                    // wrong version sends the user to reinstall something they
                    // already have; saying "wrong version" about a module
                    // published by somebody else sends them after a version
                    // that does not exist, because no version of the package
                    // they have is the package they need.
                    if (root.blockSummary === "signer")
                        return "'" + _label + "' cannot be loaded because the "
                             + "following modules were published by a different "
                             + "signer than it requires. A package under the "
                             + "right name from the wrong publisher is a "
                             + "different package — reinstall these from the "
                             + "publisher the module names:";
                    if (root.blockSummary === "mismatch")
                        return "'" + _label + "' cannot be loaded because the "
                             + "following modules are installed at a version it "
                             + "does not accept:";
                    if (root.blockSummary === "mixed")
                        return "'" + _label + "' cannot be loaded because the "
                             + "following modules are missing, are the wrong "
                             + "version, or came from a different publisher:";
                    return "'" + _label + "' cannot be loaded because the "
                         + "following modules are not installed:";
                }
                if (root.mode === "unloadCascade")
                    return "The following modules are currently loaded and depend on '"
                         + _label + "'. Unloading will terminate them:";
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
                if (root.mode === "installError") {
                    return "'" + _label + "' could not be installed. "
                         + "The package manager reported:";
                }
                return "";
            }
        }

        Rectangle {
            Layout.fillWidth: true
            color: "#1e1e1e"
            radius: 4
            border.color: "#3d3d3d"
            border.width: 1
            implicitHeight: errorMessageText.implicitHeight + 16
            visible: root.mode === "installError" && root.errorMessage.length > 0

            LogosText {
                id: errorMessageText
                anchors.fill: parent
                anchors.margins: 8
                text: root.errorMessage
                color: "#e0e0e0"
                font.pixelSize: 13
                wrapMode: Text.Wrap
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
            visible: root.mode !== "upgradeCascade"
                     && root.mode !== "installGate"
                     && root.items.length > 0

            LogosListView {
                id: itemList
                anchors.fill: parent
                anchors.margins: 8
                model: root.items
                clip: true
                delegate: LogosText {
                    readonly property string _name: root._itemName(modelData)
                    readonly property string _detail: root._itemDetail(modelData)
                    // The detail clause is what makes a version complaint
                    // actionable — a bare name tells the user a module is
                    // wrong without telling them which version to get.
                    text: "• " + (root.displayNameLookup(_name) || _name)
                          + (_detail.length > 0 ? " — " + _detail : "")
                    color: "#e0e0e0"
                    font.pixelSize: 13
                }
            }
        }

        // Loaded-dependent block for upgradeCascade. Installed-but-not-running
        // dependents pick up the new version on their next load and aren't
        // user-visibly affected, so listing them under "will stop working"
        // would be a lie — only the running ones are named. The body sentence
        // above already tells the "brief unload, then back on the new version"
        // story; this just names them.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.mode === "upgradeCascade" && root.loadedItems.length > 0

            LogosText {
                Layout.fillWidth: true
                text: "Currently running (will be temporarily unloaded):"
                color: "#c0c0c0"
                font.pixelSize: 13
                font.weight: Theme.typography.weightBold
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(120, Math.max(30, root.loadedItems.length * 24))
                color: "#1e1e1e"
                radius: 4
                border.color: "#3d3d3d"
                border.width: 1

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

            // Cancel button — hidden in the informational modes since
            // there's only one button to press there.
            //
            // Both buttons carry a mode-derived objectName so UI automation
            // can target them exactly. Text-based clicking is ambiguous here:
            // labels like "Uninstall" also appear on the module-row buttons,
            // and the inspector's text search doesn't distinguish open from
            // closed dialogs.
            //
            // These names are only stable because OverlayDialogs instantiates
            // one dialog per mode and pins `mode` declaratively. Reusing a
            // single instance across modes would silently move the handle out
            // from under the specs that click it.
            LogosButton {
                objectName: "confirmationDialog." + root.mode + ".cancel"
                text: "Cancel"
                visible: root.mode !== "missingDeps" && root.mode !== "installError"
                onClicked: {
                    root._explicitClose = true;
                    root.cancelClicked(root.moduleName);
                    root.close();
                }
            }

            LogosButton {
                objectName: "confirmationDialog." + root.mode + ".confirm"
                text: {
                    if (root.mode === "missingDeps") return "OK";
                    if (root.mode === "unloadCascade") return "Unload All";
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
        if (root.mode === "unloadCascade" || root.mode === "upgradeCascade"
            || root.mode === "installGate") {
            root.cancelClicked(root.moduleName);
        }
    }
}
