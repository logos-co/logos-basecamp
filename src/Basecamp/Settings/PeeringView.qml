import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Theme

// Settings -> Peering: this runtime's links with other Logos runtimes.
// Names and IDs other runtimes supply are shown as plain text only.
Item {
    id: root

    property var peering: ({})

    signal refreshRequested()
    signal enabledRequested(bool enabled)
    signal linkLocalRequested(string invitePath)
    signal pairRequested(string host, int port)
    signal confirmRequested(string id)
    signal rejectRequested(string id)
    signal removePeerRequested(string peer)
    signal exportsRequested(string peer)
    signal importRequested(string peer, string module, bool events)
    signal removeImportRequested(string name)

    function reportOperationResult(operation, success, error) {
        d.lastError = success ? "" : (error.length > 0 ? error : qsTr("Operation failed"))
        if (success && operation === "link") d.lastNotice = qsTr("Linking with the daemon…")
        else if (success && operation === "pair") d.lastNotice = qsTr("Compare the code below with the other screen.")
        else if (success && operation === "import") d.lastNotice = qsTr("Imported. It is ready once its state shows ready.")
        else d.lastNotice = ""
    }

    function showPeerExports(peer, exports) {
        const next = Object.assign({}, d.exportsByPeer)
        next[peer] = exports
        d.exportsByPeer = next
    }

    QtObject {
        id: d
        property string lastError: ""
        property string lastNotice: ""
        property var exportsByPeer: ({})
        property string pendingRemovePeer: ""
        property string invitePath: ""

        readonly property bool enabled: root.peering.enabled === true
        readonly property bool running: root.peering.running === true
        readonly property var status: root.peering.status || ({})
        readonly property var peers: root.peering.peers || []
        // A finished pairing stays listed a while; a paired one is shown as a peer.
        readonly property var pending: (root.peering.pending || []).filter(p => p.state !== "paired")
        readonly property var imports: root.peering.imports || []

        function imported(peer, module) {
            for (let i = 0; i < imports.length; ++i)
                if (imports[i].from === peer && imports[i].module === module) return true
            return false
        }

        function spacedCode(code) {
            const c = String(code || "")
            return c.length === 6 ? c.slice(0, 3) + " " + c.slice(3) : c
        }

        function stateColor(state) {
            if (state === "ready") return Theme.palette.success
            if (state === "error") return Theme.palette.error
            return Theme.palette.textTertiary
        }
    }

    LogosScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: Theme.spacing.large
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: Theme.spacing.large

            // Header.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.medium

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.tiny

                    LogosText {
                        text: qsTr("Peering")
                        font.pixelSize: Theme.typography.panelTitleText
                        font.weight: Theme.typography.weightMedium
                        color: Theme.palette.text
                    }
                    LogosText {
                        text: qsTr("Link this Basecamp with other Logos runtimes, such as a logosctl daemon, and use the modules they share as if they ran here.")
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textSecondary
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                LogosButton {
                    objectName: "peering.refreshButton"
                    text: qsTr("Refresh")
                    enabled: d.running
                    implicitWidth: 120
                    implicitHeight: 36
                    onClicked: root.refreshRequested()
                }
            }

            // Error banner, shown until the next successful operation.
            Rectangle {
                objectName: "peering.errorBanner"
                Layout.fillWidth: true
                visible: d.lastError.length > 0 || String(root.peering.error || "").length > 0
                radius: Theme.spacing.radiusSmall
                color: Theme.colors.getColor(Theme.palette.error, 0.12)
                border.color: Theme.palette.error
                border.width: 1
                implicitHeight: errorRow.implicitHeight + Theme.spacing.medium * 2

                RowLayout {
                    id: errorRow
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.medium
                    spacing: Theme.spacing.medium

                    LogosText {
                        Layout.fillWidth: true
                        text: d.lastError.length > 0 ? d.lastError : String(root.peering.error || "")
                        textFormat: Text.PlainText
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.secondaryText
                        wrapMode: Text.WordWrap
                    }
                    LogosIconButton {
                        iconSource: LogosIcons.close
                        size: 20
                        iconSize: 14
                        iconColor: Theme.palette.textTertiary
                        background: Item {}
                        onClicked: d.lastError = ""
                    }
                }
            }

            LogosText {
                objectName: "peering.notice"
                visible: d.lastNotice.length > 0
                Layout.fillWidth: true
                text: d.lastNotice
                color: Theme.palette.textSecondary
                font.pixelSize: Theme.typography.secondaryText
                wrapMode: Text.WordWrap
            }

            // On / off, from the next start.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.small

                LogosSwitch {
                    objectName: "peering.enabledSwitch"
                    checked: d.enabled
                    onToggled: root.enabledRequested(checked)
                }
                LogosText {
                    Layout.fillWidth: true
                    text: d.enabled ? qsTr("Linking is on") : qsTr("Linking is off")
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.primaryText
                }
                LogosBadge {
                    objectName: "peering.restartBadge"
                    visible: root.peering.restartRequired === true
                    text: qsTr("Restart Basecamp to apply")
                    color: Theme.palette.warning
                }
            }

            // This runtime.
            ColumnLayout {
                visible: d.running
                Layout.fillWidth: true
                spacing: Theme.spacing.tiny

                LogosText {
                    text: qsTr("This Basecamp")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }
                LogosText {
                    text: String(d.status.name || "")
                    textFormat: Text.PlainText
                    color: Theme.palette.textSecondary
                    font.pixelSize: Theme.typography.secondaryText
                }
                LogosSelectableText {
                    objectName: "peering.displayId"
                    text: qsTr("ID %1").arg(String(d.status.display_id || ""))
                    color: Theme.palette.textSecondary
                    font.pixelSize: Theme.typography.secondaryText
                }
            }

            // A daemon on this computer: its local invite, no code.
            ColumnLayout {
                visible: d.running
                Layout.fillWidth: true
                spacing: Theme.spacing.small

                LogosText {
                    text: qsTr("Link a daemon on this computer")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }
                LogosText {
                    Layout.fillWidth: true
                    text: root.peering.localInviteFound === true
                          ? qsTr("A daemon started with peering.control.local_invite left an invite for you here.")
                          : qsTr("No daemon invite found. Start logosctl with peering.control.local_invite, or give the invite's path.")
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textTertiary
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small

                    LogosTextField {
                        id: inviteField
                        objectName: "peering.invitePathField"
                        Layout.fillWidth: true
                        placeholderText: String(root.peering.localInvitePath || "")
                        text: d.invitePath
                        onTextChanged: if (text !== d.invitePath) d.invitePath = text
                    }
                    LogosButton {
                        objectName: "peering.linkLocalButton"
                        text: qsTr("Link")
                        implicitWidth: 100
                        implicitHeight: 40
                        onClicked: root.linkLocalRequested(d.invitePath.trim())
                    }
                }
            }

            // Another computer: a six-digit code on both screens.
            ColumnLayout {
                visible: d.running
                Layout.fillWidth: true
                spacing: Theme.spacing.small

                LogosText {
                    text: qsTr("Pair with another computer")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }
                LogosText {
                    Layout.fillWidth: true
                    text: qsTr("Open a pairing window on the other runtime first (logosctl peer pair-window). Both screens then show a six-digit code: accept only if they match.")
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textTertiary
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small

                    LogosTextField {
                        id: hostField
                        objectName: "peering.hostField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("192.168.1.5")
                    }
                    LogosTextField {
                        id: portField
                        objectName: "peering.portField"
                        Layout.preferredWidth: 100
                        text: "7443"
                        validator: IntValidator { bottom: 1; top: 65535 }
                    }
                    LogosButton {
                        objectName: "peering.pairButton"
                        text: qsTr("Pair")
                        enabled: hostField.text.trim().length > 0 && portField.textInput.acceptableInput
                        implicitWidth: 100
                        implicitHeight: 40
                        onClicked: root.pairRequested(hostField.text.trim(), parseInt(portField.text))
                    }
                }
            }

            // Pairings in progress.
            Repeater {
                model: d.pending
                delegate: Rectangle {
                    required property var modelData

                    readonly property string pairingId: String(modelData.id || "")
                    readonly property bool needsApproval: modelData.needs_approval === true

                    objectName: "peering.pending." + pairingId
                    Layout.fillWidth: true
                    implicitHeight: pendingRow.implicitHeight + Theme.spacing.large * 2
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.background
                    border.color: Theme.palette.warning
                    border.width: 1

                    RowLayout {
                        id: pendingRow
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.large
                        spacing: Theme.spacing.medium

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.tiny

                            LogosText {
                                text: String(modelData.code || "").length > 0
                                      ? qsTr("Code %1").arg(d.spacedCode(modelData.code))
                                      : qsTr("Linking by invite")
                                font.pixelSize: Theme.typography.panelTitleText
                                font.weight: Theme.typography.weightMedium
                                color: Theme.palette.text
                            }
                            LogosText {
                                Layout.fillWidth: true
                                text: qsTr("%1 · %2").arg(String(modelData.peer_name || modelData.direction || ""))
                                                     .arg(String(modelData.peer_display_id || ""))
                                textFormat: Text.PlainText
                                color: Theme.palette.textSecondary
                                font.pixelSize: Theme.typography.secondaryText
                                elide: Text.ElideRight
                            }
                            LogosText {
                                visible: String(modelData.error || "").length > 0
                                text: String(modelData.error || "")
                                textFormat: Text.PlainText
                                color: Theme.palette.error
                                font.pixelSize: Theme.typography.secondaryText
                            }
                        }
                        LogosButton {
                            visible: needsApproval
                            objectName: "peering.pending.reject." + pairingId
                            text: qsTr("Reject")
                            onClicked: root.rejectRequested(pairingId)
                        }
                        LogosButton {
                            visible: needsApproval
                            objectName: "peering.pending.accept." + pairingId
                            text: qsTr("Codes match")
                            variant: LogosButton.Variant.Primary
                            onClicked: root.confirmRequested(pairingId)
                        }
                    }
                }
            }

            Rectangle {
                visible: d.running
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.palette.borderSubtle
            }

            // Linked runtimes, and what each shares with this one.
            LogosText {
                visible: d.running
                text: qsTr("Linked runtimes")
                font.pixelSize: Theme.typography.subtitleText
                font.weight: Theme.typography.weightMedium
                color: Theme.palette.text
            }

            Repeater {
                model: d.running ? d.peers : []
                delegate: Rectangle {
                    required property var modelData

                    readonly property string peerId: String(modelData.runtime_id || "")
                    readonly property var offered: d.exportsByPeer[peerId]

                    objectName: "peering.peer." + peerId
                    Layout.fillWidth: true
                    implicitHeight: peerCol.implicitHeight + Theme.spacing.large * 2
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.background
                    border.color: Theme.palette.borderSubtle
                    border.width: 1

                    ColumnLayout {
                        id: peerCol
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.large
                        spacing: Theme.spacing.small

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.small

                            LogosText {
                                Layout.fillWidth: true
                                text: String(modelData.alias || modelData.display_name || peerId)
                                textFormat: Text.PlainText
                                font.pixelSize: Theme.typography.primaryText
                                font.weight: Theme.typography.weightMedium
                                color: Theme.palette.text
                                elide: Text.ElideRight
                            }
                            LogosBadge {
                                visible: modelData.status !== "active"
                                text: String(modelData.status || "")
                                color: Theme.palette.textTertiary
                            }
                            LogosButton {
                                objectName: "peering.peer.modules." + peerId
                                text: qsTr("Shared modules")
                                onClicked: root.exportsRequested(peerId)
                            }
                            LogosButton {
                                objectName: "peering.peer.remove." + peerId
                                text: qsTr("Remove")
                                onClicked: {
                                    d.pendingRemovePeer = peerId
                                    removePeerDialog.open()
                                }
                            }
                        }

                        LogosSelectableText {
                            Layout.fillWidth: true
                            text: qsTr("%1 · ID %2").arg(String(modelData.display_name || ""))
                                                    .arg(String(modelData.display_id || ""))
                            color: Theme.palette.textSecondary
                            font.pixelSize: Theme.typography.secondaryText
                            wrapMode: TextEdit.WrapAnywhere
                        }

                        LogosText {
                            visible: offered !== undefined && offered.length === 0
                            text: qsTr("It shares no module with this Basecamp yet.")
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.secondaryText
                        }

                        Repeater {
                            model: offered || []
                            delegate: RowLayout {
                                required property var modelData
                                readonly property string module: String(modelData.module || "")

                                Layout.fillWidth: true
                                spacing: Theme.spacing.small

                                LogosText {
                                    Layout.fillWidth: true
                                    text: module
                                    textFormat: Text.PlainText
                                    color: Theme.palette.text
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                                LogosBadge {
                                    visible: modelData.loaded !== true
                                    text: qsTr("Not running")
                                    color: Theme.palette.textTertiary
                                }
                                LogosButton {
                                    objectName: "peering.import." + peerId + "." + module
                                    text: d.imported(peerId, module) ? qsTr("Imported") : qsTr("Import")
                                    enabled: !d.imported(peerId, module)
                                    onClicked: root.importRequested(peerId, module, modelData.events === true)
                                }
                            }
                        }
                    }
                }
            }

            LogosText {
                visible: d.running && d.peers.length === 0
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Not linked with any runtime yet.")
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
            }

            // Imported modules.
            LogosText {
                visible: d.running && d.imports.length > 0
                text: qsTr("Imported modules")
                font.pixelSize: Theme.typography.subtitleText
                font.weight: Theme.typography.weightMedium
                color: Theme.palette.text
            }

            Repeater {
                model: d.running ? d.imports : []
                delegate: RowLayout {
                    required property var modelData
                    readonly property string name: String(modelData.name || "")

                    objectName: "peering.importRow." + name
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        LogosText {
                            text: qsTr("%1 from %2").arg(name).arg(String(modelData.peer_alias || modelData.from || ""))
                            textFormat: Text.PlainText
                            color: Theme.palette.text
                            font.pixelSize: Theme.typography.primaryText
                        }
                        LogosText {
                            visible: String(modelData.reason || "").length > 0
                            Layout.fillWidth: true
                            text: String(modelData.reason || "")
                            textFormat: Text.PlainText
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.secondaryText
                            wrapMode: Text.WordWrap
                        }
                    }
                    LogosBadge {
                        objectName: "peering.importState." + name
                        text: String(modelData.state || "")
                        color: d.stateColor(modelData.state)
                    }
                    LogosButton {
                        objectName: "peering.unimport." + name
                        text: qsTr("Remove")
                        onClicked: root.removeImportRequested(name)
                    }
                }
            }

            LogosText {
                visible: !d.running
                Layout.fillWidth: true
                text: d.enabled
                      ? qsTr("Linking starts with Basecamp's next start.")
                      : qsTr("Turn linking on to pair with other runtimes. It takes effect when Basecamp next starts.")
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
                wrapMode: Text.WordWrap
            }

            Item { Layout.fillHeight: true; Layout.minimumHeight: 1 }
        }
    }

    LogosWarningDialog {
        id: removePeerDialog

        anchors.centerIn: parent
        title: qsTr("Remove this link?")
        width: 420
        message: qsTr("Modules imported from it stop working, and linking again needs a new pairing.")

        leftActions: [
            LogosButton {
                objectName: "peering.removeConfirm.cancel"
                text: qsTr("Cancel")
                onClicked: {
                    d.pendingRemovePeer = ""
                    removePeerDialog.close()
                }
            }
        ]
        rightActions: [
            LogosButton {
                objectName: "peering.removeConfirm.confirm"
                text: qsTr("Remove")
                variant: LogosButton.Variant.Primary
                onClicked: {
                    root.removePeerRequested(d.pendingRemovePeer)
                    d.pendingRemovePeer = ""
                    removePeerDialog.close()
                }
            }
        ]
    }
}
