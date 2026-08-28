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
//                       Three distinct problems land here — absent, wrong
//                       version, wrong signer — with three different remedies,
//                       so the copy must not conflate them. `blockSummary`
//                       picks the sentence; each row's `detail` clause names
//                       the constraint and what was found.
//  - "unloadCascade"  — confirmation; unloading this module would leave
//                       other loaded modules stranded. Continue cascades
//                       the unload via the backend; Cancel aborts.
//  - "upgradeCascade" — confirmation; upgrading/downgrading/reinstalling
//                       this module. The old version is removed first, so
//                       currently-running dependents are unloaded for the
//                       swap; `loadedItems` names them.
//                       Installed-but-not-running dependents are NOT listed:
//                       they pick up the new version on their next load and
//                       aren't user-visibly affected. Title and body lead
//                       with the new version + the UpgradeMode (Upgrade /
//                       Downgrade / Reinstall) so the user knows the
//                       operation isn't a bare uninstall. Confirm/Cancel flow
//                       through `continueClicked` / `cancelClicked`.
//  - "installGate"    — confirmation before a fresh install, raised as the
//                       `logos.packages.confirm_install` intent. Every install
//                       the app performs comes through here: package_manager_ui
//                       initiates them all, whether the source is a catalog
//                       download or a local .lgx the user picked. Leads with the
//                       package + target version and lists the resolved
//                       transitive `depChanges`. No dependent-impact lists (a
//                       fresh install unloads nothing). Confirm/Cancel flow
//                       through continueClicked / cancelClicked →
//                       confirmInstallGate / cancelInstallGate, which answer
//                       the intent; PMU then performs the install.
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
// blockSummary)` for the one-list modes, `openWithUpgrade(name, version,
// upgradeMode, installedDeps, loadedDeps, depChanges, requester)` for
// upgradeCascade, or `openWithInstallGate(name, version, depChanges,
// requester)` for the install gate. Backend wiring listens for
// continueClicked/cancelClicked and calls the appropriate slot with `name`.
Dialog {
    id: root

    // "missingDeps" | "unloadCascade" | "upgradeCascade" | "installGate" | "installError"
    property string mode: "missingDeps"
    property string moduleName: ""
    // For "missingDeps" each entry is a map from
    // logos::dependencyBlockerToMap — {name, kind, requiredVersion,
    // installedVersion, requiredSigner, signerDid, detail}. For the other
    // one-list modes it is a plain module name; `_itemName` / `_itemDetail`
    // read either shape.
    property var items: []
    // missingDeps only: "" | "absent" | "mismatch" | "signer" | "mixed".
    // Computed host-side (logos::summariseDependencyBlockers) so one set of
    // blockers yields one sentence everywhere it is described.
    property string blockSummary: ""
    // Only used in upgradeCascade mode — the dependents currently loaded,
    // which get torn down for the version swap.
    property var loadedItems: []
    // Only used in upgradeCascade mode. `upgradeTargetVersion` is the pinned
    // target (e.g. "1.0.0") from the confirm_upgrade payload;
    // `upgradeModeKind` mirrors PackageTypes/UpgradeMode —
    //   0 = Upgrade, 1 = Downgrade, 2 = Sidegrade (Reinstall).
    property string upgradeTargetVersion: ""
    property int upgradeModeKind: 0

    // Transitive dependency changes for the upgradeCascade + installGate modes.
    // Each entry: { name, action: "install"|"upgrade"|"downgrade",
    //               fromVersion, toVersion, repository }. Resolved by the SHELL,
    //               never sent by the requester — confirm_install is open to any
    //               app, and a caller-supplied list would let it script this
    //               dialog. Empty = nothing else needs to change.
    property var depChanges: []

    property string errorMessage: ""

    // The app that asked, host-attested. Empty when the user acted through
    // the shell's own UI, where naming a requester would be noise.
    property string requesterName: ""
    // True when that app is embedded — it came out of our own bundle, which is
    // provenance the shell can vouch for. Drives whether the line WARNS or
    // merely attributes; a warning on every ordinary operation is noise that
    // teaches the user to skip the one that matters.
    property bool requesterBundled: false

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

    // An `items` entry is either a blocker map or a bare module name. Read it
    // through these, never by stringifying: a map stringifies to an empty
    // label without complaint.
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
    function openWithUpgrade(name_, version_, upgradeMode_, installedDeps_, loadedDeps_, depChanges_, requester_, requesterBundled_) {
        root.mode = "upgradeCascade";
        root.moduleName = name_ || "";
        root.upgradeTargetVersion = version_ || "";
        root.upgradeModeKind = upgradeMode_ | 0;
        root.items = installedDeps_ || [];
        root.loadedItems = loadedDeps_ || [];
        root.depChanges = depChanges_ || [];
        root.requesterName = requester_ || "";
        root._explicitClose = false;
        open();
    }

    // Fresh catalog-install variant. Unlike upgradeCascade there is no
    // dependent-impact set (nothing is uninstalled/unloaded); the dialog
    // simply confirms the install and lists the transitive `depChanges`.
    // Continue / Cancel still flow through continueClicked / cancelClicked;
    // the backend routes those to confirmInstallGate / cancelInstallGate.
    function openWithInstallGate(name_, version_, depChanges_, requester_, requesterBundled_) {
        root.mode = "installGate";
        root.moduleName = name_ || "";
        root.upgradeTargetVersion = version_ || "";
        root.items = [];
        root.loadedItems = [];
        root.depChanges = depChanges_ || [];
        root.requesterName = requester_ || "";
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
                        // Named, not defaulted: "Missing Dependencies" is
                        // the fallback only for shapes where something really
                        // is absent. Any other shape would inherit a title
                        // contradicting its own body text.
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

        // Who asked. Shown for requests that crossed an app boundary — the
        // package name, never the self-declared display name, so an app cannot
        // dress itself up as another. Nothing is signed, so this is provenance
        // for the channel, not proof of a publisher.
        LogosText {
            Layout.fillWidth: true
            visible: root.requesterName.length > 0
            wrapMode: Text.Wrap
            // Payload-adjacent and attacker-influenced on the open
            // confirm_install intent: never let a name render as markup.
            color: Theme.palette.textSecondary
            font.pixelSize: Theme.typography.secondaryText
            // Bundled: the display name is enough — it came out of our bundle,
            // so the label is as trustworthy as the rest of the app, and a
            // module id here would just be noise.
            //
            // Not bundled: BOTH. The display name is self-declared, so showing
            // it alone would let "Wallet" front for evil_ui; showing the id
            // alone is unreadable. Same treatment as IntentChooserDialog — the
            // id is appended only when it actually differs.
            readonly property string _who: root.displayNameLookup(root.requesterName)
                                           || root.requesterName
            text: root.requesterBundled
                  ? qsTr("Requested by %1, which ships with Logos.").arg(_who)
                  : (_who === root.requesterName
                     ? qsTr("Requested by %1 — not part of Logos, and unsigned, so the shell cannot confirm who published it.")
                         .arg(_who)
                     : qsTr("Requested by %1 (%2) — not part of Logos, and unsigned, so the shell cannot confirm who published it.")
                         .arg(_who).arg(root.requesterName))
        }

        LogosText {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.palette.textSecondary
            readonly property string _label: root.displayNameLookup(root.moduleName) || root.moduleName
            text: {
                if (root.mode === "missingDeps") {
                    // Four facts, four sentences. "Not installed" about a
                    // module installed at the wrong version sends the user to
                    // reinstall what they have; "wrong version" about somebody
                    // else's package sends them after a version that does not
                    // exist. The signer sentence may be this strong because it
                    // reports a failed Ed25519 check against the key the module
                    // itself named, not a record the installer wrote down.
                    if (root.blockSummary === "signer")
                        return "'" + _label + "' cannot be loaded because the "
                             + "following modules are not signed by the key it "
                             + "requires. A package under the right name signed "
                             + "by a different key is a different package — "
                             + "reinstall these from the publisher the module "
                             + "names:";
                    if (root.blockSummary === "mismatch")
                        return "'" + _label + "' cannot be loaded because the "
                             + "following modules are installed at a version it "
                             + "does not accept:";
                    if (root.blockSummary === "mixed")
                        return "'" + _label + "' cannot be loaded because the "
                             + "following modules are missing, are the wrong "
                             + "version, or are not signed by the key it "
                             + "requires:";
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
                    // The detail clause is what makes the complaint
                    // actionable: a bare name does not say which version, or
                    // whose package, to go and get.
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
