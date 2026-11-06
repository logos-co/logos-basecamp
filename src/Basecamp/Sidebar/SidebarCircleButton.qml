import QtQuick
import QtQuick.Controls
import Logos.Theme
import Logos.Controls

AbstractButton {
    id: root

    implicitHeight: 38
    implicitWidth: 38

    property bool stopped: false

    signal tooltipRequested(string text, real y)

    onHoveredChanged: {
        if (hovered && text) {
            var pos = root.mapToItem(null, root.width, root.height / 2)
            root.tooltipRequested(text, pos.y)
        }
    }

    // Dark gray pill background extending to left edge when active/highlighted
    background: Rectangle {
        radius: width / 2
        color: root.hovered || root.checked ? Theme.palette.accentOrange: Theme.palette.surface
    }

    contentItem: Item {
        LogosIcon {
            id: appIcon
            anchors.centerIn: parent
            width: 24
            height: 24
            source: root.icon.source
            color: root.hovered || root.checked ? Theme.palette.backgroundBlack: "transparent"
            // Null when there is no source; the initials stand in then.
            visible: appIcon.imageItem.status !== Image.Null
                     && appIcon.imageItem.status !== Image.Error
        }
        LogosText {
            objectName: "sidebar.initials"
            anchors.centerIn: parent
            text: root.text.substring(0, 4)
            font.pixelSize: Theme.typography.secondaryText
            font.weight: Theme.typography.weightBold
            // On the accent fill, the icon's ink: textSecondary was ~1:1 on it.
            color: root.hovered || root.checked ? Theme.palette.backgroundBlack
                                                : Theme.palette.textSecondary
            visible: !appIcon.visible
        }
        BrokenAppMarker {
            objectName: "sidebar.marker.stopped"
            visible: root.stopped
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: -2
            anchors.topMargin: -2
        }
    }
}
