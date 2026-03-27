import QtQuick
import QtQuick.Controls
import QtQuick.Effects

import Logos.Theme
import Logos.Controls

AbstractButton {
    id: root

    property bool loaded: false
    property bool loading: false

    implicitHeight: 50

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
    }
}
