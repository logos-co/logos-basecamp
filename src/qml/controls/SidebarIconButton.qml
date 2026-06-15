import QtQuick
import QtQuick.Controls
import Logos.Theme
import Basecamp.Icons

AbstractButton {
    id: root

    implicitHeight: 46
    checkable: true
    autoExclusive: true

    onHoveredChanged: {
        if (hovered && text) {
            var pos = root.mapToItem(null, root.width, root.height / 2)
            backend.sidebarTooltipY = pos.y
            backend.sidebarTooltipText = root.text
        } else {
            backend.sidebarTooltipText = ""
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
