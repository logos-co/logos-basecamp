import QtQuick
import QtQuick.Controls
import Logos.Theme
import Logos.Controls

AbstractButton {
    id: root

    // Corner marker for an informational, non-blocking state — currently an
    // available host-app update on the Settings button. Same shape as
    // SidebarAppDelegate's missing-deps marker minus the ✕ glyph, in the
    // primary accent rather than red so it never reads as an error.
    property bool showNotificationDot: false

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

        // The -2 corner margins put the dot's centre at (33, 5), which lands
        // on the 38px pill's own 45° edge point (32.4, 5.6) — so it hugs the
        // circle rather than floating in the empty bounding-box corner.
        Rectangle {
            id: notificationDot
            visible: root.showNotificationDot
            width: 14
            height: 14
            radius: 7
            color: Theme.palette.primary
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: -2
            anchors.topMargin: -2
        }
    }
}
