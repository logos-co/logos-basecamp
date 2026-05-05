import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Theme

// Reusable dialog for dependency-aware confirmation / informational prompts.
//
// Four display variants, selected via `mode`:
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
//  - "installConfirm" — confirmation before installing / upgrading an LGX
//                       file. Renders package metadata (name, version, type,
//                       signature status). For upgrades, shows the installed
//                       version alongside the new version.
//
// The dialog is controlled by calling `openWith(mode, name, items)` for the
// one-list modes, `openWithTwoLists(mode, name, items, loadedItems)` for
// uninstallCascade, or `openWithMetadata(metadata)` for installConfirm.
// Backend wiring listens for continueClicked/cancelClicked and calls the
// appropriate slot with `name`.
Dialog {
    id: root

    // "missingDeps" | "unloadCascade" | "uninstallCascade" | "installConfirm"
    property string mode: "missingDeps"
    property string moduleName: ""
    property var items: []
    // Only used in uninstallCascade mode — the subset of `items` that is
    // currently loaded and will be torn down as part of the cascade.
    property var loadedItems: []
    // Only used in installConfirm mode — full QVariantMap from inspectPackage.
    property var metadata: ({})
    // Only used in uninstallCascade for the multi-package variant
    property var targets: []

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
    width: 440
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

    // Metadata variant for installConfirm — metadata is the QVariantMap from
    // inspectPackage containing name, version, type, signatureStatus, etc.
    function openWithMetadata(metadata_) {
        root.mode = "installConfirm";
        root.moduleName = (metadata_ && metadata_.name) ? metadata_.name : "";
        root.metadata = metadata_ || {};
        root.items = [];
        root.loadedItems = [];
        root.targets = [];
        root._explicitClose = false;
        open();
    }

    background: Rectangle {
        color: "#2d2d2d"
        border.color: "#3d3d3d"
        border.width: 1
        radius: 8
    }

    contentItem: ColumnLayout {
        spacing: 12

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
                if (root.mode === "installConfirm") {
                    if (root.metadata.isAlreadyInstalled)
                        return "Upgrade Package?";
                    return "Install Package?";
                }
                return "";
            }
            font.pixelSize: 18
            font.weight: Font.Bold
            color: "#ffffff"
            wrapMode: Text.Wrap
        }

        LogosText {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: "#c0c0c0"
            text: {
                if (root.mode === "missingDeps")
                    return "'" + root.moduleName + "' cannot be loaded because the "
                         + "following modules are not installed:";
                if (root.mode === "unloadCascade")
                    return "The following modules are currently loaded and depend on '"
                         + root.moduleName + "'. Unloading will terminate them:";
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
                        return "Uninstall '" + root.moduleName + "'? This will "
                             + "remove the package files from disk.";
                    return "Uninstalling '" + root.moduleName + "' will remove the "
                         + "package files from disk and affect the following:";
                }
                if (root.mode === "installConfirm") {
                    if (root.metadata.isAlreadyInstalled)
                        return "An existing version is installed. Review the details "
                             + "below and confirm the upgrade:";
                    return "Review the package details below before installing:";
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
                     && root.mode !== "installConfirm"
                     && root.items.length > 0

            ListView {
                id: itemList
                anchors.fill: parent
                anchors.margins: 8
                model: root.items
                clip: true
                delegate: LogosText {
                    text: "• " + modelData
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
                font.weight: Font.Bold
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(150, Math.max(30, root.targets.length * 24))
                color: "#1e1e1e"
                radius: 4
                border.color: "#3d3d3d"
                border.width: 1

                ListView {
                    anchors.fill: parent
                    anchors.margins: 8
                    model: root.targets
                    clip: true
                    delegate: LogosText {
                        text: "• " + modelData
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
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.mode === "uninstallCascade"
                     && (root.items.length > 0 || root.loadedItems.length > 0)

            // Installed dependents (full impact — will break on next load).
            LogosText {
                Layout.fillWidth: true
                text: "Will stop working (installed but not running):"
                color: "#c0c0c0"
                font.pixelSize: 13
                font.weight: Font.Bold
                wrapMode: Text.Wrap
                visible: root.items.length > 0
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(120, Math.max(30, root.items.length * 24))
                color: "#1e1e1e"
                radius: 4
                border.color: "#3d3d3d"
                border.width: 1
                visible: root.items.length > 0

                ListView {
                    anchors.fill: parent
                    anchors.margins: 8
                    model: root.items
                    clip: true
                    delegate: LogosText {
                        text: "• " + modelData
                        color: "#e0e0e0"
                        font.pixelSize: 13
                    }
                }
            }

            // Loaded dependents (will be unloaded now).
            LogosText {
                Layout.fillWidth: true
                Layout.topMargin: 4
                text: "Will be unloaded now:"
                color: "#c0c0c0"
                font.pixelSize: 13
                font.weight: Font.Bold
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

                ListView {
                    anchors.fill: parent
                    anchors.margins: 8
                    model: root.loadedItems
                    clip: true
                    delegate: LogosText {
                        text: "• " + modelData
                        color: "#e0e0e0"
                        font.pixelSize: 13
                    }
                }
            }
        }

        // Metadata block for installConfirm — renders package details as a
        // key-value grid inside a tinted box. Shows version comparison when
        // upgrading an already-installed package.
        Rectangle {
            Layout.fillWidth: true
            color: "#1e1e1e"
            radius: 4
            border.color: "#3d3d3d"
            border.width: 1
            visible: root.mode === "installConfirm"
            implicitHeight: metadataCol.implicitHeight + 16

            ColumnLayout {
                id: metadataCol
                anchors.fill: parent
                anchors.margins: 8
                spacing: 4

                // Shortens a long hex hash ("abcd1234...abcd1234") for display.
                // Matches the PMU details-panel format so users see the same
                // fingerprint on both surfaces.
                function shortHash(h) {
                    if (!h) return "";
                    if (h.length <= 16) return h;
                    return h.substring(0, 8) + "…" + h.substring(h.length - 8);
                }

                // Helper component for key-value rows
                component MetadataRow: RowLayout {
                    property string label
                    property string value
                    Layout.fillWidth: true
                    spacing: 8
                    visible: value !== ""
                    LogosText {
                        text: label
                        color: "#909090"
                        font.pixelSize: 13
                        Layout.preferredWidth: 120
                    }
                    LogosText {
                        text: parent.value
                        color: "#e0e0e0"
                        font.pixelSize: 13
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                    }
                }

                MetadataRow {
                    label: "Name:"
                    value: root.metadata.name || ""
                }
                MetadataRow {
                    label: "Version:"
                    value: root.metadata.version || ""
                    visible: !root.metadata.isAlreadyInstalled && (root.metadata.version || "") !== ""
                }
                MetadataRow {
                    label: "Hash:"
                    value: metadataCol.shortHash(root.metadata.rootHash || "")
                    visible: !root.metadata.isAlreadyInstalled && (root.metadata.rootHash || "") !== ""
                }
                MetadataRow {
                    label: "Installed:"
                    value: {
                        var v = root.metadata.installedVersion || "";
                        var h = metadataCol.shortHash(root.metadata.installedHash || "");
                        if (v && h) return v + " (" + h + ")";
                        return v;
                    }
                    visible: root.metadata.isAlreadyInstalled === true
                }
                MetadataRow {
                    label: "New version:"
                    value: {
                        var v = root.metadata.version || "";
                        var h = metadataCol.shortHash(root.metadata.rootHash || "");
                        if (v && h) return v + " (" + h + ")";
                        return v;
                    }
                    visible: root.metadata.isAlreadyInstalled === true
                }
                MetadataRow {
                    label: "Type:"
                    value: root.metadata.type || ""
                }
                MetadataRow {
                    label: "Category:"
                    value: root.metadata.category || ""
                }
                MetadataRow {
                    label: "Description:"
                    value: root.metadata.description || ""
                }

                // Signature row — icon + colored text for status
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    visible: (root.metadata.signatureStatus || "") !== ""
                    LogosText {
                        text: "Signature:"
                        color: "#909090"
                        font.pixelSize: 13
                        Layout.preferredWidth: 120
                    }
                    LogosText {
                        font.pixelSize: 13
                        Layout.fillWidth: true
                        text: {
                            var s = root.metadata.signatureStatus || "";
                            if (s === "signed") {
                                var name = root.metadata.signerName || root.metadata.signerDid || "";
                                return name ? "\u2713 Signed by " + name : "\u2713 Signed";
                            }
                            if (s === "unsigned") return "\u26A0 Unsigned";
                            if (s === "invalid")  return "\u2717 Invalid signature";
                            if (s === "error")    return "\u2717 Signature error";
                            return s;
                        }
                        color: {
                            var s = root.metadata.signatureStatus || "";
                            if (s === "signed")   return "#4CAF50";
                            if (s === "unsigned")  return "#FFC107";
                            if (s === "invalid" || s === "error") return "#f44336";
                            return "#e0e0e0";
                        }
                    }
                }
            }
        }

        // Dependents block for installConfirm upgrades — shown when the
        // package being upgraded has currently-loaded dependents. Unlike
        // uninstall, an upgrade preserves installed-but-not-running
        // dependents (they'll pick up the new module on their next load),
        // so we only surface the loaded ones here — those need an explicit
        // unload before the new module is swapped in.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.mode === "installConfirm"
                     && root.metadata.isAlreadyInstalled === true
                     && _installLoadedDeps && _installLoadedDeps.length > 0

            readonly property var _installLoadedDeps: root.metadata.loadedDependents || []

            LogosText {
                Layout.fillWidth: true
                text: "Will be unloaded for the upgrade:"
                color: "#c0c0c0"
                font.pixelSize: 13
                font.weight: Font.Bold
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(100, Math.max(30, parent._installLoadedDeps.length * 24))
                color: "#1e1e1e"
                radius: 4
                border.color: "#3d3d3d"
                border.width: 1

                ListView {
                    anchors.fill: parent
                    anchors.margins: 8
                    model: parent.parent._installLoadedDeps
                    clip: true
                    delegate: LogosText {
                        text: "\u2022 " + modelData
                        color: "#e0e0e0"
                        font.pixelSize: 13
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
            Button {
                text: "Cancel"
                visible: root.mode !== "missingDeps"

                contentItem: LogosText {
                    text: parent.text
                    font.pixelSize: 13
                    color: "#ffffff"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    implicitWidth: 90
                    implicitHeight: 32
                    color: parent.pressed ? "#3d3d3d" : "#4b4b4b"
                    radius: 4
                }

                onClicked: {
                    root._explicitClose = true;
                    if (root.targets.length > 0)
                        root.cancelClickedMulti(root.targets);
                    else
                        root.cancelClicked(root.moduleName);
                    root.close();
                }
            }

            Button {
                text: {
                    if (root.mode === "missingDeps") return "OK";
                    if (root.mode === "unloadCascade") return "Unload All";
                    if (root.mode === "uninstallCascade") {
                        if (root.targets.length > 1)
                            return "Uninstall " + root.targets.length;
                        return "Uninstall";
                    }
                    if (root.mode === "installConfirm") {
                        if (root.metadata.isAlreadyInstalled) return "Upgrade";
                        return "Install";
                    }
                    return "OK";
                }

                contentItem: LogosText {
                    text: parent.text
                    font.pixelSize: 13
                    color: "#ffffff"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    implicitWidth: 100
                    implicitHeight: 32
                    color: {
                        // Destructive actions get a red background to match
                        // the Uninstall button in the modules tabs.
                        if (root.mode === "uninstallCascade")
                            return parent.pressed ? "#da190b" : "#f44336";
                        if (root.mode === "unloadCascade")
                            return parent.pressed ? "#da190b" : "#f44336";
                        // Upgrade gets an amber accent; fresh install gets green.
                        if (root.mode === "installConfirm" && root.metadata.isAlreadyInstalled)
                            return parent.pressed ? "#e68900" : "#FF9800";
                        return parent.pressed ? "#45a049" : "#4CAF50";
                    }
                    radius: 4
                }

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
            || root.mode === "installConfirm") {
            if (root.targets.length > 0)
                root.cancelClickedMulti(root.targets);
            else
                root.cancelClicked(root.moduleName);
        }
    }
}
