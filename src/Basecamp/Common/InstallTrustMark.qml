import QtQuick

import Logos.Controls
import Logos.Icons
import Logos.Theme

Item {
    id: root

    property var verdict
    property bool showsTooltip: true

    // The mark in words: its tooltip.
    readonly property string description: {
        if (d.installed) {
            switch (d.status) {
            case "bundled":
                return qsTr("Ships with Basecamp")
            case "trusted":
                return qsTr("Already installed, signed by %1").arg(verdict.publisher || qsTr("a publisher you trust"))
            case "untrusted":
                return qsTr("Already installed, signed by a publisher you don't trust")
            case "unsigned":
                return qsTr("Already installed, not signed")
            }
            return qsTr("Already installed; couldn't check who signed it")
        }
        if (d.reserved)
            return qsTr("Comes with Basecamp, so only Logos can update it")
        if (d.replaces.length > 0)
            return qsTr("Not signed by %1, who signed the version you have").arg(d.replaces)
        if (d.replacesUnverified)
            return qsTr("The version you have isn't signed, so this may not be from the same publisher")
        switch (d.status) {
        case "bundled":
            return qsTr("Ships with Basecamp")
        case "trusted":
            return qsTr("Signed by %1").arg(verdict.publisher || qsTr("a publisher you trust"))
        case "untrusted":
            return verdict.impersonates
                   ? qsTr("Claims to be %1, but isn't signed with its key").arg(verdict.impersonates)
                   : qsTr("Signed by a publisher you don't trust yet")
        case "unsigned":
            return qsTr("Not signed: nobody vouches for it")
        case "invalid":
            return qsTr("Damaged, or its signature doesn't hold")
        }
        return qsTr("Couldn't check who signed it")
    }

    visible: d.status.length > 0
    implicitWidth: 14
    implicitHeight: 14

    QtObject {
        id: d

        readonly property string status: root.verdict ? (root.verdict.status || "") : ""
        // Shown by a tap, for screens with no hover.
        property bool tapped: false
        readonly property bool installed: !!root.verdict && root.verdict.installed === true
        // Not what you have, or not anyone's but Logos's to replace.
        readonly property bool reserved: !!root.verdict && root.verdict.reserved === true
        readonly property string replaces: root.verdict ? (root.verdict.replaces || "") : ""
        readonly property bool replacesUnverified: !!root.verdict && root.verdict.replacesUnverified === true
        readonly property color tint: {
            if (d.installed)
                return Theme.palette.textTertiary
            if (d.reserved || d.replaces.length > 0 || d.replacesUnverified)
                return Theme.palette.error
            switch (d.status) {
            case "bundled":
            case "trusted":
                return Theme.palette.success
            case "invalid":
                return Theme.palette.error
            case "untrusted":
                return root.verdict.impersonates ? Theme.palette.error : Theme.palette.warning
            }
            return Theme.palette.warning
        }
    }

    LogosIcon {
        objectName: "installTrust.markIcon"
        anchors.fill: parent
        source: (d.status === "trusted" || d.status === "bundled") && d.replaces.length === 0
                && !d.replacesUnverified ? LogosIcons.check : LogosIcons.warning
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
