import QtQuick
import QtQuick.Controls

import Logos.Theme
import Logos.Controls

Control {
    id: root

    property int spinnerSize: 36

    z: 1
    padding: 0

    background: Rectangle {
        color: Theme.colors.getColor(Theme.palette.background, 0.65)
    }

    contentItem: Item {
        LogosSpinner {
            id: spinner
            anchors.centerIn: parent
            implicitWidth: root.spinnerSize
            implicitHeight: root.spinnerSize
            running: root.visible
        }
    }

    MouseArea {
        id: blocker
        anchors.fill: parent
    }
}
