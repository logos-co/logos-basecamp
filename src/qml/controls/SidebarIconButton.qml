import QtQuick
import QtQuick.Controls
import Logos.Theme

AbstractButton {
    id: root

    implicitHeight: 46
    checkable: true
    autoExclusive: true

    background: Image {
        width: 56
        height: 46
        anchors.centerIn: parent
        source: "qrc:/icons/workspace.png"
        fillMode: Image.PreserveAspectFit
    }

    contentItem: Item {
        Image {
            anchors.centerIn: parent
            width: 24
            height: 24
            source: root.icon.source
            fillMode: Image.PreserveAspectFit
        }
    }
}
