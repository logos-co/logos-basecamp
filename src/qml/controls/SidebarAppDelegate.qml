import QtQuick
import QtQuick.Controls
import QtQuick.Effects

import Logos.Theme
import Logos.Controls

AbstractButton {
    id: root

    property bool loaded : false

    implicitHeight: 50

    background: Rectangle {
        color: root.loaded ? Theme.palette.backgroundTertiary: Theme.palette.overlayDark
        Rectangle {
            anchors.left: parent.left
            width: 3
            height: parent.height
            color: root.checked ? Theme.palette.accentOrange: "transparent"
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
            visible: !!root.icon.source &&
                     !(appIcon.status === Image.Null ||
                       appIcon.status === Image.Error)
        }
        MultiEffect {
            anchors.fill: appIcon
            source: appIcon
            colorization: 1.0
            colorizationColor: "transparent"
            brightness: root.checked ? 0.5: 0
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
