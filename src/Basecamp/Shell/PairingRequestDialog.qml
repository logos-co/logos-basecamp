import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Theme

// Another runtime asks to pair with this one (peering_module's pairingRequested).
// A code pairing is accepted only when both screens show the same code; a
// different code means someone in between. An operator invite is compared by
// the redeemer's ID, which `logosctl remote pair` prints on the other side.
//
// Peer strings are plain text. Requests queue; syncWith() drops one answered or
// expired elsewhere. Escape leaves it pending in Settings -> Peering.
IntentDialog {
    id: root
    objectName: "pairingRequestDialog"

    // The request on screen: {id, code, peer_name, peer_display_id, role, expires_ms}.
    readonly property alias request: d.current
    readonly property alias queued: d.queue

    signal confirmRequested(string id)
    signal rejectRequested(string id)

    function openWith(request) {
        const id = String((request || {}).id || "")
        if (id.length === 0 || id === String(d.current.id || "") || d.indexIn(d.queue, id) >= 0) return
        const entry = Object.assign({ receivedAt: Date.now() }, request)
        if (visible) {
            d.queue = d.queue.concat([entry])
            return
        }
        d.show(entry)
    }

    // `pending` is peering_module's list. A request it listed and no longer
    // does was answered or expired; one it has not listed yet may be newer
    // than the list.
    function syncWith(pending) {
        const list = pending || []
        const seen = Object.assign({}, d.seen)
        for (let i = 0; i < list.length; ++i) seen[String(list[i].id || "")] = true
        d.seen = seen
        const gone = id => d.seen[id] === true && d.indexIn(list, id) < 0
        d.queue = d.queue.filter(r => !gone(String(r.id)))
        if (visible && gone(String(d.current.id || ""))) close()
    }

    title: d.operator ? qsTr("Let another computer operate this runtime?")
                      : qsTr("Pair with another runtime?")

    onClosed: {
        d.current = ({})
        d.next()
    }

    QtObject {
        id: d
        property var current: ({})
        property var queue: []
        property var seen: ({})
        readonly property bool operator: String(current.role || "") === "operator"

        function indexIn(list, id) {
            for (let i = 0; i < list.length; ++i)
                if (String(list[i].id || "") === id) return i
            return -1
        }
        function show(request) {
            const ttl = Number(request.expires_ms || 0)
            const left = ttl - (Date.now() - request.receivedAt)
            if (ttl > 0 && left <= 0) {
                next() // expired while queued
                return
            }
            current = request
            expiry.stop()
            if (ttl > 0) {
                expiry.interval = left
                expiry.start()
            }
            root.open()
        }
        function next() {
            if (queue.length === 0) return
            const request = queue[0]
            queue = queue.slice(1)
            show(request)
        }
        function spaced(code) {
            const c = String(code || "")
            return c.length === 6 ? c.slice(0, 3) + " " + c.slice(3) : c
        }
    }

    // It expires on the other side too; nothing is left to answer.
    Timer {
        id: expiry
        onTriggered: if (root.visible) root.close()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing.medium

        LogosText {
            objectName: "pairingRequestBody"
            Layout.fillWidth: true
            text: d.operator
                  ? qsTr("%1 asks to operate this runtime: to load, unload and call its modules.")
                        .arg(String(d.current.peer_name || qsTr("Another computer")))
                  : qsTr("%1 asks to pair with this Basecamp.")
                        .arg(String(d.current.peer_name || qsTr("Another runtime")))
            textFormat: Text.PlainText
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textSecondary
            wrapMode: Text.Wrap
        }

        LogosText {
            objectName: "pairingRequestCode"
            visible: !d.operator
            Layout.alignment: Qt.AlignHCenter
            text: d.spaced(d.current.code)
            textFormat: Text.PlainText
            font.pixelSize: Theme.typography.panelTitleText * 2
            font.weight: Theme.typography.weightBold
            font.letterSpacing: 2
            color: Theme.palette.text
        }

        LogosSelectableText {
            objectName: "pairingRequestDisplayId"
            Layout.fillWidth: true
            text: qsTr("ID %1").arg(String(d.current.peer_display_id || ""))
            font.pixelSize: d.operator ? Theme.typography.primaryText : Theme.typography.secondaryText
            color: d.operator ? Theme.palette.text : Theme.palette.textSecondary
        }

        LogosText {
            Layout.fillWidth: true
            text: d.operator
                  ? qsTr("Accept only if you started this from the other computer and it shows the same ID.")
                  : qsTr("Accept only if the other screen shows the same code. Pairing shares nothing by itself: what each side imports is chosen afterwards.")
            font.pixelSize: Theme.typography.secondaryText
            color: d.operator ? Theme.palette.warning : Theme.palette.textSubtle
            wrapMode: Text.Wrap
        }
    }

    rightActions: [
        LogosButton {
            objectName: "pairingRequestReject"
            text: qsTr("Reject")
            onClicked: {
                root.rejectRequested(String(d.current.id))
                root.close()
            }
        },
        LogosButton {
            objectName: "pairingRequestAccept"
            text: d.operator ? qsTr("Same ID, accept") : qsTr("Codes match")
            variant: LogosButton.Variant.Primary
            onClicked: {
                root.confirmRequested(String(d.current.id))
                root.close()
            }
        }
    ]
}
