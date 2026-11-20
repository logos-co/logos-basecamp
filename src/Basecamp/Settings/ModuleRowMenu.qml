import QtQuick

import Logos.Controls

// ModuleRowActions as a menu, for an inspector table's phone cards: the
// row's detail, then the same actions with the same rules, opened from a
// card's ⋯ for `row` (the table's menuRow).
LogosMenu {
    id: root

    property var row: null
    property bool interfaceEnabled: false
    property bool busy: false
    property bool locked: false

    signal openRequested()
    signal loadToggleRequested()
    signal interfaceRequested()

    LogosMenuItem {
        objectName: "moduleRowMenu.open"
        text: qsTr("Open details")
        onTriggered: root.openRequested()
    }

    LogosMenuItem {
        objectName: "moduleRowMenu.loadToggle"
        // A hidden item still takes its height in the menu's list.
        visible: !!root.row && !root.row.isMainUi
        height: visible ? implicitHeight : 0
        enabled: !root.busy && !root.locked
        text: root.row && root.row.isLoaded ? qsTr("Unload") : qsTr("Load")
        onTriggered: root.loadToggleRequested()
    }

    LogosMenuItem {
        objectName: "moduleRowMenu.interface"
        visible: root.interfaceEnabled && !!root.row && root.row.isLoaded
        height: visible ? implicitHeight : 0
        text: qsTr("Interface")
        onTriggered: root.interfaceRequested()
    }
}
