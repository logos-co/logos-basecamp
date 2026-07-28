import QtQuick
import QtQuick.Controls
import Logos.Theme
import Logos.Controls

AbstractButton {
    id: root

    implicitHeight: 38
    implicitWidth: 38

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
            visible: !!root.icon.source &&
                     !(appIcon.status === Image.Null ||
                       appIcon.status === Image.Error)
        }
        LogosText {
            anchors.centerIn: parent
            text: root.text.substring(0, 4)
            font.pixelSize: Theme.typography.secondaryText
            font.weight: Theme.typography.weightBold
            color: Theme.palette.textSecondary
            visible: !appIcon.visible
        }
    }
}
