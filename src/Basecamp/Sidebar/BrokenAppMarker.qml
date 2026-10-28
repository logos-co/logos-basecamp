import QtQuick

import Logos.Theme

// Corner marker on an app that cannot run. Red cross: a dependency is missing,
// or the app's backend stopped. Amber "!": every dependency is present but one
// is rejected — the user resolves that differently, so it must not look the same.
Rectangle {
    id: root

    property bool exclamation: false

    width: 14
    height: 14
    radius: 7
    color: root.exclamation ? Theme.palette.warning : Theme.palette.error

    Rectangle {
        visible: !root.exclamation
        width: 8
        height: 1.5
        color: Theme.colors.white
        anchors.centerIn: parent
        rotation: 45
    }
    Rectangle {
        visible: !root.exclamation
        width: 8
        height: 1.5
        color: Theme.colors.white
        anchors.centerIn: parent
        rotation: -45
    }

    Rectangle {
        visible: root.exclamation
        width: 1.5
        height: 5
        radius: 0.75
        color: Theme.colors.white
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 3
    }
    Rectangle {
        visible: root.exclamation
        width: 1.5
        height: 1.5
        radius: 0.75
        color: Theme.colors.white
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 3
    }
}
