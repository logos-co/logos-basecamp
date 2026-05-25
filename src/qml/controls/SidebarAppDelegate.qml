import QtQuick
import QtQuick.Controls
import QtQuick.Effects

import Logos.Theme
import Logos.Controls

AbstractButton {
    id: root

    property bool loaded: false
    property bool loading: false
    // True iff the backend reports this plugin has unmet core dependencies.
    // When set, a red-cross overlay renders top-right — clicking still emits
    // `clicked`, and the backend decides whether to load or show the popup.
    property bool hasMissingDeps: false

    implicitHeight: 50

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
        Image {
            id: appIcon
            anchors.centerIn: parent
            width: 24
            height: 24
            source: root.icon.source
            fillMode: Image.PreserveAspectFit
            visible: !root.loading
                     && !!root.icon.source
                     && !(appIcon.status === Image.Null || appIcon.status === Image.Error)
        }
        MultiEffect {
            anchors.fill: appIcon
            source: appIcon
            colorization: 1.0
            colorizationColor: "transparent"
            brightness: root.checked ? 0.5 : 0
            visible: appIcon.visible
        }
        LogosText {
            anchors.centerIn: parent
            text: root.text.substring(0, 4)
            font.pixelSize: Theme.typography.secondaryText
            font.weight: Theme.typography.weightBold
            color: Theme.palette.textSecondary
            visible: !root.loading && !appIcon.visible
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

        // Missing-dependencies overlay: small red circle with a white "x",
        // drawn with QML primitives so we don't need to ship a new asset.
        // Anchored top-right of the delegate; hidden while the plugin is
        // loading (spinner animates there otherwise the two overlap).
        Rectangle {
            id: missingDepsMarker
            visible: root.hasMissingDeps && !root.loading
            width: 14
            height: 14
            radius: 7
            color: "#d32f2f" // material red 700 — not in the current theme palette
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 6
            anchors.topMargin: 6

            // White "x" — two thin rectangles rotated 45°/-45° inside the circle.
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
