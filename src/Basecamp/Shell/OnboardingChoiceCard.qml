import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

LogosFrame {
    id: root

    property string title: ""
    property string description: ""
    property string badge: ""
    property bool selected: false
    property bool multi: false

    signal picked()

    Layout.fillWidth: true
    radius: Theme.spacing.radiusLarge
    backgroundColor: Theme.palette.surfaceRaised
    borderColor: Theme.palette.border
    padding: 0

    contentItem: Item {
        implicitWidth: row.implicitWidth + 2 * Theme.spacing.large
        implicitHeight: row.implicitHeight + 2 * Theme.spacing.large

        RowLayout {
            id: row
            anchors.fill: parent
            anchors.margins: Theme.spacing.large
            spacing: Theme.spacing.medium

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small

                    LogosText {
                        text: root.title
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.primaryText
                        font.weight: Theme.typography.weightBold
                    }

                    LogosBadge {
                        visible: root.badge.length > 0
                        text: root.badge
                        color: Theme.palette.success
                    }

                    Item { Layout.fillWidth: true }
                }

                LogosText {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: root.description
                    color: Theme.palette.textSecondary
                    font.pixelSize: Theme.typography.secondaryText
                    wrapMode: Text.WordWrap
                }
            }

            LogosRadioButton {
                objectName: "choiceIndicator"
                Layout.alignment: Qt.AlignVCenter
                visible: !root.multi
                checked: root.selected
            }

            LogosCheckbox {
                objectName: "choiceCheckbox"
                Layout.alignment: Qt.AlignVCenter
                visible: root.multi
                checked: root.selected
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.picked()
        }
    }
}
