import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

// An inspector row's detail, laid out as Alisher's table detail: its status
// and version, its description, then its fields as rows of a card, the ones
// the table drops for room among them. Its name is in the side panel's bar.
LogosScrollView {
    id: root

    property var row: null
    property string label: ""          // what Basecamp calls it
    property string name: ""           // the module's own name, when not the label
    property string version: ""
    property string description: ""
    // [[name, value]]; a field without a value is left out.
    property var fields: []

    contentWidth: availableWidth
    clip: true

    QtObject {
        id: d
        readonly property int pad: Theme.pageMargin(root.width)
        readonly property var shownFields: root.fields.filter(field => field[1] !== "")
    }

    Item {
        implicitWidth: root.availableWidth
        implicitHeight: column.implicitHeight + 2 * d.pad

        ColumnLayout {
            id: column
            x: d.pad
            y: d.pad
            width: parent.width - 2 * d.pad
            spacing: Theme.spacing.large

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.tiny

                // Its status, version and own name; a name wraps rather than
                // lose any of itself.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small
                    ModuleStatusBadge {
                        Layout.alignment: Qt.AlignTop
                        row: root.row
                    }
                    LogosText {
                        objectName: "moduleDetail.subtitle"
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: [root.version !== "" ? "v" + root.version : "",
                               root.name !== root.label ? root.name : ""]
                              .filter(part => part !== "").join(" · ")
                        color: Theme.palette.textSecondary
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                }
            }

            LogosText {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.description
                wrapMode: Text.Wrap
                color: Theme.palette.textSecondary
            }

            Rectangle {
                Layout.fillWidth: true
                visible: d.shownFields.length > 0
                implicitHeight: rows.implicitHeight
                radius: Theme.spacing.radiusXlarge
                color: Theme.palette.backgroundSecondary
                clip: true

                ColumnLayout {
                    id: rows
                    width: parent.width
                    spacing: 0

                    Repeater {
                        model: d.shownFields
                        delegate: LogosListRow {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            title: modelData[0]
                            value: modelData[1]
                            showSeparator: index < d.shownFields.length - 1
                        }
                    }
                }
            }
        }
    }
}
