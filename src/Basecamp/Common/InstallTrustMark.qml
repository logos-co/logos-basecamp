import QtQuick

import Logos.Controls
import Logos.Icons
import Logos.Theme

import Basecamp.Installer

Item {
    id: root

    property var verdict
    property bool showsTooltip: true

    // The mark in words (the shared Trust.js): its tooltip.
    readonly property string description: Trust.description(root.verdict)

    visible: d.status.length > 0
    implicitWidth: 14
    implicitHeight: 14

    QtObject {
        id: d

        readonly property string status: root.verdict ? (root.verdict.status || "") : ""
        // Shown by a tap, for screens with no hover.
        property bool tapped: false
        readonly property color tint: {
            switch (Trust.tone(root.verdict)) {
            case "fine":
                return Theme.palette.success
            case "danger":
                return Theme.palette.error
            case "muted":
                return Theme.palette.textTertiary
            }
            return Theme.palette.warning
        }
    }

    LogosIcon {
        objectName: "installTrust.markIcon"
        anchors.fill: parent
        source: Trust.isCheck(root.verdict) ? LogosIcons.check : LogosIcons.warning
        brightness: 1.0
        color: d.tint
    }

    Accessible.role: Accessible.Graphic
    Accessible.name: description

    HoverHandler { id: hover }
    TapHandler {
        enabled: root.showsTooltip
        margin: Theme.spacing.small
        onTapped: d.tapped = !d.tapped
    }
    LogosToolTip {
        objectName: "installTrust.markTip"
        text: root.description
        placement: LogosToolTip.Top
        visible: root.showsTooltip && (hover.hovered || d.tapped)
    }
}
