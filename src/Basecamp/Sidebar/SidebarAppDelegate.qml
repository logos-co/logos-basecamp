import QtQuick
import QtQuick.Controls

import Logos.Theme
import Logos.Controls
import Basecamp.AppManager

AbstractButton {
    id: root

    property bool loaded: false
    property bool loading: false
    // True iff the backend reports this plugin has unmet core dependencies.
    // When set, a red-cross overlay renders top-right — clicking still emits
    // `clicked`, and the backend decides whether to load or show the popup.
    property bool hasMissingDeps: false

    implicitHeight: 50

    QtObject {
        id: d

        readonly property string nameText: root.text || ""
        readonly property string monogram: (d.nameText || "?").substring(0, 2).toUpperCase()
        readonly property color tileColor: AppColors.colorForApp(d.nameText)
        readonly property real uninstalledTileAlpha: 0.55
        readonly property color tileBackgroundColor:
            root.loaded ? d.tileColor
                        : Theme.colors.getColor(d.tileColor, d.uninstalledTileAlpha)

        readonly property int tileSize: 38
        readonly property int iconSize: 19
        readonly property int monogramSize: Math.round(d.tileSize * 0.375)
    }

    onHoveredChanged: {
        if (hovered && text) {
            var pos = root.mapToItem(null, root.width, root.height / 2)
            backend.sidebarTooltipY = pos.y
            backend.sidebarTooltipText = root.text
        } else {
            backend.sidebarTooltipText = ""
        }
    }

    background: Rectangle {
        color: root.loaded ? Theme.palette.backgroundTertiary : Theme.palette.overlayDark
        Rectangle {
            anchors.left: parent.left
            width: 3
            height: parent.height
            color: root.checked ? Theme.palette.accentOrange : "transparent"
        }
    }

    contentItem: Item {
        Rectangle {
            id: tile
            anchors.centerIn: parent
            width: d.tileSize
            height: d.tileSize
            radius: Theme.spacing.radiusMedium
            color: d.tileBackgroundColor

            LogosIcon {
                id: appIcon
                anchors.centerIn: parent
                source: root.icon.source
                color: Theme.palette.text
                brightness: 1.0
                width: d.iconSize
                height: d.iconSize
                visible: !root.loading
                         && !!root.icon.source
                         && appIcon.imageItem.status !== Image.Null
                         && appIcon.imageItem.status !== Image.Error
            }

            LogosText {
                anchors.centerIn: parent
                text: d.monogram
                font.pixelSize: d.monogramSize
                font.weight: Theme.typography.weightBold
                color: Theme.palette.text
                visible: !root.loading && !appIcon.visible
            }
        }

        Item {
            id: spinner
            anchors.centerIn: parent
            width: 20
            height: 20
            visible: root.loading

            RotationAnimator on rotation {
                running: spinner.visible
                from: 0
                to: 360
                duration: 900
                loops: Animation.Infinite
            }

            Repeater {
                model: 8
                Rectangle {
                    required property int index
                    width: 3
                    height: 3
                    radius: 1.5
                    color: Theme.palette.textSecondary
                    opacity: (index + 1) / 8
                    x: spinner.width / 2 + 7 * Math.cos(index * Math.PI / 4) - width / 2
                    y: spinner.height / 2 + 7 * Math.sin(index * Math.PI / 4) - height / 2
                }
            }
        }

        Rectangle {
            id: missingDepsMarker
            visible: root.hasMissingDeps && !root.loading
            width: 14
            height: 14
            radius: 7
            color: "#d32f2f"
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 6
            anchors.topMargin: 6

            Rectangle {
                width: 8
                height: 1.5
                color: "white"
                anchors.centerIn: parent
                rotation: 45
            }
            Rectangle {
                width: 8
                height: 1.5
                color: "white"
                anchors.centerIn: parent
                rotation: -45
            }
        }
    }
}
