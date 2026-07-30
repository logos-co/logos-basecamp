import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

// Trailing action cluster for an inspector table row: load/unload toggle plus
// an optional Interface drill-down. Inspectors are view-only; management
// actions (Uninstall) live in the Package Manager, so this control never
// exposes them.
//
// Visibility rules:
//   * main_ui never gets a load toggle — that module is this app.
//   * Interface only renders for loaded modules, and only where the host view
//     has somewhere to navigate to (`interfaceEnabled`).
RowLayout {
    id: root

    property var row: null
    property bool interfaceEnabled: false
    property bool busy: false

    signal loadToggleRequested()
    signal interfaceRequested()

    spacing: Theme.spacing.small

    LogosButton {
        Layout.preferredWidth: 100
        Layout.preferredHeight: 40
        radius: Theme.spacing.radiusLarge
        visible: root.row && !root.row.isMainUi
        enabled: !root.busy
        text: root.row && root.row.isLoaded ? qsTr("Unload") : qsTr("Load")
        variant: root.row && root.row.isLoaded ? LogosButton.Variant.Secondary
                                              : LogosButton.Variant.Primary
        onClicked: root.loadToggleRequested()
    }

    LogosButton {
        Layout.preferredWidth: 100
        Layout.preferredHeight: 40
        radius: Theme.spacing.radiusLarge
        visible: root.interfaceEnabled && root.row && root.row.isLoaded
        text: qsTr("Interface")
        onClicked: root.interfaceRequested()
    }
}
