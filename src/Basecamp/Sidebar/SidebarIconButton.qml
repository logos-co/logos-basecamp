import QtQuick
import QtQuick.Controls
import Logos.Theme
import Basecamp.Icons

AbstractButton {
    id: root

    implicitHeight: 46
    checkable: true
    autoExclusive: true

    signal tooltipRequested(string text, real y)
    signal tooltipCleared()

    onHoveredChanged: {
        if (hovered && text) {
            var pos = root.mapToItem(null, root.width, root.height / 2)
            root.tooltipRequested(text, pos.y)
        } else {
            root.tooltipCleared()
        }
    }

    background: Image {
        width: 56
        height: 46
        anchors.centerIn: parent
        source: BasecampIcons.workspace
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
