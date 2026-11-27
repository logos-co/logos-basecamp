import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Theme
import Basecamp.Common

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
//                       `basecamp.packages.confirm_install` intent. Every install
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
//  - "loadFailed"     — informational; a UI app failed to load (e.g. its QML
//                       view did not compile). Shows the app's display name +
//                       the reported error in a scrollable, selectable area so
//                       multi-line QML errors can be read and copied. OK only.
//
// Uninstall confirmation is NOT here — it lives in UninstallDialog.qml, which
// renders a resolved plan (what goes, what stays and why, what breaks) rather
// than a pair of dependent lists.
//
// A LogosWarningDialog, as UninstallDialog is: centred and 560 wide, and in a
// phone-wide window a sheet across the bottom, which must be answered.
//
// The dialog is controlled by calling `openWith(mode, name, items,
// blockSummary)` for the one-list modes, `openWithUpgrade(name, version,
// upgradeMode, installedDeps, loadedDeps, depChanges, requester)` for
// upgradeCascade, or `openWithInstallGate(name, version, depChanges,
// requester)` for the install gate, `openWithInstallError(name, error)` /
// `openWithLoadError(name, error)` for the error modes. Backend wiring listens for
// continueClicked/cancelClicked and calls the appropriate slot with `name`.
LogosWarningDialog {
    id: root

    // "missingDeps" | "unloadCascade" | "upgradeCascade" | "installGate" | "installError"
    // | "loadFailed"
    property string mode: "missingDeps"
    property string moduleName: ""
    // The request the dialog answers; empty for Basecamp's own.
    property string requestId: ""
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
    readonly property var mandatoryChanges: depChanges.filter(function(c) { return !c.optional; })
    readonly property var optionalPackages: depChanges.filter(function(c) { return c.optional; })
    property var optionalSelection: ({})
    property var optionalPickedVersions: ({})
    property bool resolutionPending: false
    signal optionalPreviewRequested(string name, var selection, var versionPins)

    function refreshPreview() {
        resolutionPending = true;
        optionalPreviewRequested(moduleName, optionalSelection, optionalPickedVersions);
    }

    // Whether `p` can be installed at `version`, from a source that serves it.
    function offersVersion(p, version) {
        return !p.error && !!version && (version === p.version
            || (p.versions || []).some(function(v) {
                return v && v.manifest && v.manifest.version === version && v.sourceAvailable !== false;
            }));
    }

    onOptionalPackagesChanged: {
        var picks = {};
        optionalPackages.forEach(function(p) {
            if (offersVersion(p, optionalPickedVersions[p.name])) picks[p.name] = optionalPickedVersions[p.name];
        });
        optionalPickedVersions = picks;
    }

    // Each optional as the requester asked: ticked or not, and at the version
    // it picked (`requestedVersion`), which is what the plan resolved with.
    function resetOptionalSelection() {
        var selected = {};
        var picks = {};
        for (var i = 0; i < optionalPackages.length; ++i) {
            var offer = optionalPackages[i];
            if (!offer.error && !offer.requiredFor) selected[offer.name] = offer.selected !== false;
            if (offersVersion(offer, offer.requestedVersion)) picks[offer.name] = offer.requestedVersion;
        }
        resolutionPending = false;
        optionalSelection = selected;
        optionalPickedVersions = picks;
    }

    // The backend's default (`selected`) until the user toggles the row.
    function isOptionalSelected(p) {
        if (p.error || p.requiredFor) return false;
        if (p.installedOnly) return true;
        const own = optionalSelection[p.name];
        return own !== undefined ? own : p.selected !== false;
    }

    function selectedOptionalNames() {
        return optionalPackages.filter(function(p) {
            return isOptionalSelected(p);
        }).map(function(p) { return p.name; });
    }
    // Whether `depChanges` is a CONCLUSION or just an absence. An empty list
    // means "nothing needs to change" only when the resolver actually ran;
    // when it could not (no repository to resolve against, or the call
    // failed) the gate knows nothing, and must not say otherwise.
    property bool depChangesResolved: true

    property string errorMessage: ""
    // "loadFailed" only: replaces "Couldn't Load App" (a startup notice about a module).
    property string loadFailedTitle: ""

    // The app that asked, host-attested. Empty when the user acted through
    // the shell's own UI, where naming a requester would be noise.
    property string requesterName: ""
    // True when that app is embedded — it came out of our own bundle, which is
    // provenance the shell can vouch for. Drives whether the line WARNS or
    // merely attributes; a warning on every ordinary operation is noise that
    // teaches the user to skip the one that matters.
    property bool requesterBundled: false
    // App Manager, the Apps slot's app (DefaultApps.h): its requests are the
    // person's own clicks there, so the dialog speaks of an app and says
    // nothing of who asked.
    property string appsApp: ""
    readonly property bool appWording: root.requesterName.length > 0
                                       && root.requesterName === root.appsApp

    property var displayNameLookup: function(name) { return name; }

    // Internal: tracks whether a button explicitly handled the close
    // so the onClosed handler doesn't double-fire cancelClicked. Set
    // true in continue/cancel onClicked before we call close().
    property bool _explicitClose: false

    // installGate / upgradeCascade: who signed what this would install
    // (PackageCoordinator::installTrustUpdated). Empty means nothing to say.
    property var trust: ({})

    signal continueClicked(string name)
    signal cancelClicked(string name)
    signal trustAndContinueClicked(string name)

    QtObject {
        id: d

        readonly property bool trustApplies: root.mode === "installGate" || root.mode === "upgradeCascade"
        // What the change stops while it swaps files: what runs on the package
        // it upgrades (loadedItems), and what each package it replaces stops
        // (a row's `closes`), for the rows that install: an optional one when
        // ticked, or when one that needs it is.
        readonly property var running: {
            const names = (root.loadedItems || []).slice()
            const add = function(c) {
                (c.closes || []).forEach(function(n) { if (names.indexOf(n) < 0) names.push(n) })
            }
            root.mandatoryChanges.forEach(add)
            const ticked = root.selectedOptionalNames()
            root.optionalPackages.forEach(function(p) {
                if (ticked.indexOf(p.name) >= 0
                    || (p.requiredFor || []).some(function(n) { return ticked.indexOf(n) >= 0 }))
                    add(p)
            })
            return names
        }
        readonly property bool trustHolds: d.trustApplies
                                           && (root.trust.pending === true || root.trust.blocked === true)
        readonly property bool trustAsks: d.trustApplies && !d.trustHolds && root.trust.needsPrompt === true
        readonly property bool canTrustPublishers: d.trustAsks && root.trust.canTrust === true
        readonly property string anyway: d.trustAsks && root.trust.trustCoversAll !== true ? " anyway" : ""

        // One pass, unlike chained arg(): a name that reads "%2" stays as it is.
        function fill(template, values) {
            return template.replace(/%([1-9])/g, (match, n) => n <= values.length ? values[n - 1] : match);
        }

        // A package's verdict (an InstallTrustMark on its row), once checked.
        function verdictFor(name) {
            return d.trustApplies && root.trust.byName ? root.trust.byName[name] : undefined;
        }
        // Who signed the package this asks about, said in the sentence that
        // names it ("Install 'x' v1, signed by Logos.") rather than on a line
        // of its own that reads as if it were about the list under it.
        function trustClause() {
            const v = d.verdictFor(root.moduleName);
            if (!v) return "";
            if (v.builtIn)
                return ", which comes with Basecamp and updates only with it";
            if (v.reserved)
                return ", which comes with Basecamp, so only Logos can update it";
            if (v.backendTaken)
                return ", whose backend's name another installed package already has";
            if (v.replaces)
                return ", which isn't signed by " + v.replaces + ", who signed the version you have";
            if (v.replacesUnverified)
                return ", which may not be from the same publisher as the version you have";
            switch (v.status) {
            case "trusted":
                return ", signed by " + (v.publisher || "a publisher you trust");
            case "untrusted":
                return v.impersonates
                       ? ", which claims to be " + v.impersonates + " but isn't signed with its key"
                       : ", signed by a publisher you don't trust yet";
            case "unsigned":
                return ", which isn't signed";
            case "invalid":
                return ", which is damaged or wrongly signed";
            }
            return " (couldn't check who signed it)";
        }
    }

    width: 560
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape
    // A neutral frame, the warning in red.
    accentColor: Theme.palette.error
    borderColor: Theme.palette.border
    backgroundColor: Theme.palette.surfaceRaised

    title: {
        if (root.mode === "missingDeps") {
            if (root.blockSummary === "signer")   return "Unexpected Publisher";
            if (root.blockSummary === "mismatch") return "Incompatible Dependencies";
            return "Missing Dependencies";
        }
        if (root.mode === "unloadCascade")
            return "Unload Dependent Modules?";
        if (root.mode === "upgradeCascade") {
            if (root.upgradeModeKind === 1) return root.appWording ? "Downgrade App?" : "Downgrade Package?";
            if (root.upgradeModeKind === 2) return root.appWording ? "Reinstall App?" : "Reinstall Package?";
            return root.appWording ? "Upgrade App?" : "Upgrade Package?";
        }
        if (root.mode === "installGate")
            return root.appWording ? "Install App?" : "Install Package?";
        if (root.mode === "installError")
            return "Install Failed";
        if (root.mode === "loadFailed")
            return root.loadFailedTitle || "Couldn't Load App";
        return "";
    }

    rightActions: [
        LogosButton {
            objectName: "confirmationDialog." + root.mode + ".cancel"
            text: "Cancel"
            visible: root.mode !== "missingDeps" && root.mode !== "installError"
                     && root.mode !== "loadFailed"
            onClicked: {
                root._explicitClose = true;
                root.cancelClicked(root.moduleName);
                root.close();
            }
        },

        LogosButton {
            objectName: "confirmationDialog." + root.mode + ".confirm"
            text: {
                if (root.mode === "missingDeps") return "OK";
                if (root.mode === "unloadCascade") return "Unload All";
                if (root.mode === "upgradeCascade") {
                    if (root.upgradeModeKind === 1) return "Downgrade" + d.anyway;
                    if (root.upgradeModeKind === 2) return "Reinstall" + d.anyway;
                    return "Upgrade" + d.anyway;
                }
                if (root.mode === "installGate") return "Install" + d.anyway;
                return "OK";
            }
            variant: d.canTrustPublishers ? LogosButton.Variant.Secondary
                                              : LogosButton.Variant.Primary
            enabled: !root.resolutionPending && !d.trustHolds
                     && !root.mandatoryChanges.some(function(c) { return !!c.error; })
            onClicked: {
                root._explicitClose = true;
                root.continueClicked(root.moduleName);
                root.close();
            }
        },

        LogosButton {
            objectName: "confirmationDialog." + root.mode + ".trustAndConfirm"
            visible: d.canTrustPublishers
            text: {
                if (root.mode === "upgradeCascade") {
                    if (root.upgradeModeKind === 1) return "Trust & downgrade" + d.anyway;
                    if (root.upgradeModeKind === 2) return "Trust & reinstall" + d.anyway;
                    return "Trust & upgrade" + d.anyway;
                }
                return "Trust & install" + d.anyway;
            }
            variant: LogosButton.Variant.Primary
            enabled: !root.resolutionPending && !root.mandatoryChanges.some(function(c) { return !!c.error; })
            onClicked: {
                root._explicitClose = true;
                root.trustAndContinueClicked(root.moduleName);
                root.close();
            }
        }
    ]

    // API for parent components — simpler than setting props + open() each time.
    function openWith(mode_, name_, items_, blockSummary_) {
        root.mode = mode_;
        root.moduleName = name_ || "";
        root.requestId = "";
        root.items = items_ || [];
        root.blockSummary = blockSummary_ || "";
        root.loadedItems = [];
        root._explicitClose = false;
        open();
    }

    // Request `requestId_` ended unanswered: the dialog showing it closes, and
    // answers nothing. A cancel would go by name, to whatever asks about the
    // same package next.
    function withdraw(requestId_) {
        if (!root.visible || !requestId_ || requestId_ !== root.requestId) return false;
        root._explicitClose = true;
        close();
        return true;
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
    function openWithUpgrade(name_, version_, upgradeMode_, installedDeps_, loadedDeps_, depChanges_, requester_, requesterBundled_,
                             requestId_) {
        root.mode = "upgradeCascade";
        root.trust = ({});
        root.moduleName = name_ || "";
        root.requestId = requestId_ || "";
        root.upgradeTargetVersion = version_ || "";
        root.upgradeModeKind = upgradeMode_ | 0;
        root.items = installedDeps_ || [];
        root.loadedItems = loadedDeps_ || [];
        root.depChanges = depChanges_ || [];
        root.resetOptionalSelection();
        root.requesterName = requester_ || "";
        root.requesterBundled = !!requesterBundled_;
        root._explicitClose = false;
        open();
    }

    // Fresh catalog-install variant. Unlike upgradeCascade there is no
    // dependent-impact set (nothing is uninstalled/unloaded); the dialog
    // simply confirms the install and lists the transitive `depChanges`.
    // Continue / Cancel still flow through continueClicked / cancelClicked;
    // the backend routes those to confirmInstallGate / cancelInstallGate.
    function openWithInstallGate(name_, version_, depChanges_, requester_, requesterBundled_,
                                depChangesResolved_, requestId_) {
        root.mode = "installGate";
        root.trust = ({});
        root.requestId = requestId_ || "";
        // Defaults to true so existing callers keep today's meaning.
        root.depChangesResolved = depChangesResolved_ === undefined
                                  ? true : !!depChangesResolved_;
        root.moduleName = name_ || "";
        root.upgradeTargetVersion = version_ || "";
        root.items = [];
        root.loadedItems = [];
        root.depChanges = depChanges_ || [];
        root.resetOptionalSelection();
        root.requesterName = requester_ || "";
        root.requesterBundled = !!requesterBundled_;
        root._explicitClose = false;
        open();
    }

    function openWithInstallError(name_, errorMessage_) {
        root.mode = "installError";
        root.moduleName = name_ || "";
        root.requestId = "";
        root.errorMessage = errorMessage_ || "";
        root.items = [];
        root.loadedItems = [];
        root.depChanges = [];
        root._explicitClose = false;
        open();
    }

    // Load-failure variant — informational only. `error_` is shown verbatim
    // (QML compile errors are multi-line).
    function openWithLoadError(name_, error_, title_) {
        root.mode = "loadFailed";
        root.moduleName = name_ || "";
        root.requestId = "";
        root.errorMessage = (error_ || "").trim();
        root.loadFailedTitle = title_ || "";
        root.items = [];
        root.loadedItems = [];
        root.depChanges = [];
        root._explicitClose = false;
        open();
    }

    body: ColumnLayout {
        spacing: Theme.spacing.medium

        // Who asked. Shown for requests that crossed an app boundary — the
        // package name, never the self-declared display name, so an app cannot
        // dress itself up as another. Which app asked, not who published it.
        LogosText {
            objectName: "confirmationDialog.requester"
            Layout.fillWidth: true
            visible: root.requesterName.length > 0 && !root.appWording
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
                  ? d.fill(qsTr("Requested by %1, which ships with Logos."), [_who])
                  : (_who === root.requesterName
                     ? d.fill(qsTr("Requested by %1, which is not part of Logos."), [_who])
                     : d.fill(qsTr("Requested by %1 (%2), which is not part of Logos."),
                              [_who, root.requesterName]))
        }

        LogosText {
            objectName: "confirmationDialog.body"
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
                    var head = verb + " '" + _label + "'" + verPhrase + d.trustClause()
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
                    if (d.running.length === 0)
                        return head;
                    return head + " The modules below are running on top of it and "
                                + "will be unloaded for the swap — they keep working "
                                + "with the new version once it lands.";
                }
                if (root.mode === "installGate") {
                    var ivPhrase = root.upgradeTargetVersion.length > 0
                                   ? " v" + root.upgradeTargetVersion : "";
                    var iHead = "Install '" + _label + "'" + ivPhrase + d.trustClause() + ".";
                    // The dep-change list below spells out the transitive set.
                    // When it's empty, say so plainly so a bare install still
                    // reads as a deliberate, complete confirmation.
                    if (root.mandatoryChanges.length === 0) {
                        // Only a resolved empty set licenses the claim.
                        if (!root.depChangesResolved)
                            return iHead + " Its dependencies could not be "
                                 + "determined, so other packages may be "
                                 + "installed as well.";
                        return iHead + (root.optionalPackages.length > 0
                            ? " Choose optional packages below."
                            : " No other packages need to change.");
                    }
                    // What runs on a package it replaces stops for the swap.
                    if (d.running.length > 0)
                        return iHead + " Installing it also applies the dependency "
                                     + "changes listed below. The modules below are running "
                                     + "on ones it replaces and will be unloaded for the swap.";
                    return iHead + " Installing it also applies the dependency "
                                 + "changes listed below:";
                }
                if (root.mode === "installError") {
                    return "'" + _label + "' could not be installed. "
                         + "The package manager reported:";
                }
                if (root.mode === "loadFailed")
                    return "'" + _label + "' failed to load. Details:";
                return "";
            }
        }

        // Error details for loadFailed — scrollable + selectable so long,
        // multi-line QML compile errors can be read and copied into a bug
        // report.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(220, Math.max(60, loadErrorText.implicitHeight + 2 * Theme.spacing.small))
            color: Theme.palette.surfaceRecessed
            radius: Theme.spacing.radiusSmall
            border.color: Theme.palette.border
            border.width: Theme.size.borderWidth
            visible: root.mode === "loadFailed"

            ScrollView {
                id: loadErrorScroll
                anchors.fill: parent
                anchors.margins: Theme.spacing.small
                clip: true

                TextArea {
                    id: loadErrorText
                    objectName: "confirmationDialog." + root.mode + ".error"
                    width: loadErrorScroll.availableWidth
                    text: root.errorMessage.length > 0 ? root.errorMessage
                                                       : "No error details were reported."
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    selectByMouse: true
                    selectByKeyboard: true
                    selectionColor: Theme.palette.accentOrange
                    wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                    font.pixelSize: Theme.typography.secondaryText
                    font.family: Theme.typography.mono
                    color: Theme.palette.text
                    padding: 0
                    background: Rectangle { color: "transparent" }
                }
            }
        }

        Rectangle {
            objectName: "confirmationDialog." + root.mode + ".message"
            Layout.fillWidth: true
            color: Theme.palette.surfaceRecessed
            radius: Theme.spacing.radiusSmall
            border.color: Theme.palette.border
            border.width: Theme.size.borderWidth
            implicitHeight: errorMessageText.implicitHeight + 2 * Theme.spacing.small
            visible: root.mode === "installError" && root.errorMessage.length > 0

            LogosText {
                id: errorMessageText
                anchors.fill: parent
                anchors.margins: Theme.spacing.small
                text: root.errorMessage
                color: Theme.palette.text
                font.pixelSize: Theme.typography.primaryText
                wrapMode: Text.Wrap
            }
        }

        // Single-list block — used by missingDeps and unloadCascade.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(150, itemList.contentHeight + 2 * Theme.spacing.small)
            color: Theme.palette.surfaceRecessed
            radius: Theme.spacing.radiusSmall
            border.color: Theme.palette.border
            border.width: Theme.size.borderWidth
            visible: root.mode !== "upgradeCascade"
                     && root.mode !== "installGate"
                     && root.items.length > 0

            LogosListView {
                id: itemList
                objectName: "confirmationDialog.items"
                anchors.fill: parent
                anchors.margins: Theme.spacing.small
                model: root.items
                clip: true
                delegate: LogosText {
                    readonly property string _name: root._itemName(modelData)
                    readonly property string _detail: root._itemDetail(modelData)
                    // The detail clause is what makes the complaint
                    // actionable: a bare name does not say which version, or
                    // whose package, to go and get.
                    width: ListView.view ? ListView.view.width : 0
                    text: "• " + (root.displayNameLookup(_name) || _name)
                          + (_detail.length > 0 ? " — " + _detail : "")
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.primaryText
                    wrapMode: Text.Wrap
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
            spacing: Theme.spacing.small
            visible: d.trustApplies && d.running.length > 0

            LogosText {
                Layout.fillWidth: true
                text: "Currently running (will be temporarily unloaded):"
                color: Theme.palette.textSecondary
                font.pixelSize: Theme.typography.primaryText
                font.weight: Theme.typography.weightBold
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(120, runningList.contentHeight + 2 * Theme.spacing.small)
                color: Theme.palette.surfaceRecessed
                radius: Theme.spacing.radiusSmall
                border.color: Theme.palette.border
                border.width: Theme.size.borderWidth

                LogosListView {
                    id: runningList
                    objectName: "confirmationDialog.running"
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.small
                    model: d.running
                    clip: true
                    delegate: LogosText {
                        width: ListView.view ? ListView.view.width : 0
                        text: "• " + (root.displayNameLookup(modelData) || modelData)
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.primaryText
                        wrapMode: Text.Wrap
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
                     && root.mandatoryChanges.length > 0

            LogosText {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacing.tiny
                text: "Dependency changes:"
                color: Theme.palette.textSecondary
                font.pixelSize: Theme.typography.primaryText
                font.weight: Theme.typography.weightBold
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                color: Theme.palette.surfaceRecessed
                radius: Theme.spacing.radiusSmall
                border.color: Theme.palette.border
                border.width: Theme.size.borderWidth
                implicitHeight: depChangeList.implicitHeight + 2 * Theme.spacing.small

                LogosListView {
                    id: depChangeList
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.small
                    model: root.mandatoryChanges
                    spacing: Theme.spacing.tiny
                    clip: true
                    interactive: contentHeight > height
                    implicitHeight: Math.min(160, Math.max(contentHeight, 0))

                    delegate: RowLayout {
                        width: ListView.view ? ListView.view.width : 0
                        spacing: Theme.spacing.small

                        // Action chip — colour-keyed like the PMU row pills.
                        Rectangle {
                            Layout.preferredWidth: 76
                            Layout.preferredHeight: 20
                            radius: Theme.spacing.radiusPill
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
                                font.pixelSize: Theme.typography.secondaryText
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
                            color: Theme.palette.text
                            font.pixelSize: Theme.typography.primaryText
                            elide: Text.ElideMiddle
                            objectName: "confirmationDialog.change." + (modelData.name || "")
                            Layout.maximumWidth: Math.ceil(implicitWidth)
                        }

                        InstallTrustMark {
                            objectName: "confirmationDialog.trust." + modelData.name
                            verdict: d.verdictFor(modelData.name)
                        }

                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: (root.mode === "installGate" || root.mode === "upgradeCascade")
                     && root.optionalPackages.length > 0
            LogosText {
                text: qsTr("Optional Packages")
                font.weight: Theme.typography.weightBold
            }
            LogosText {
                Layout.fillWidth: true
                text: qsTr("Selected packages and their required dependencies will also be installed.")
                wrapMode: Text.Wrap
                color: Theme.palette.textSecondary
            }
            LogosListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(root.optionalPackages.length * 56, 168)
                clip: true
                model: root.optionalPackages
                delegate: PackageRowDelegate {
                    required property var modelData
                    width: ListView.view ? ListView.view.width : 0
                    height: 56
                    appRow: modelData
                    // A package that comes only with an optional: no checkbox of its own.
                    selectable: !modelData.requiredFor
                    selectionObjectName: "confirmationDialog.optional." + modelData.name
                    selected: root.isOptionalSelected(modelData)
                    selectedVersion: root.optionalPickedVersions[modelData.name] || ""
                    trustVerdict: d.verdictFor(modelData.name)
                    onSelectionToggled: function(checked) {
                        var selected = Object.assign({}, root.optionalSelection);
                        selected[modelData.name] = checked;
                        root.optionalSelection = selected;
                        root.refreshPreview();
                    }
                    onVersionPicked: function(name, version) {
                        var picks = Object.assign({}, root.optionalPickedVersions);
                        picks[name] = version;
                        root.optionalPickedVersions = picks;
                        root.refreshPreview();
                    }
                }
            }
        }

        InstallTrustNotice {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacing.small
            trust: d.trustApplies ? root.trust : ({})
        }

        OutsideDependencyNotice {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacing.small
            dependencies: d.trustApplies ? (root.trust.outsideDependencies || []) : []
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
