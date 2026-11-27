import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Layouts
import Logos.Theme

Rectangle {
    id: root

    // ─── Public API ───
    property string themeId: ""
    property string name: ""
    property string author: ""
    property bool builtin: false
    property bool current: false
    // The theme's own colours, for the preview.
    property color background: "transparent"
    property color accent: "transparent"

    signal selectRequested()
    signal removeRequested()

    objectName: "appearance.row." + themeId
    implicitHeight: rowLayout.implicitHeight + Theme.spacing.small * 2
    radius: Theme.spacing.radiusLarge
    color: Theme.palette.background
    border.color: root.current ? Theme.palette.primary : Theme.palette.borderSubtle
    border.width: Theme.size.borderWidth

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: if (!root.current) root.selectRequested()
    }

    // The Built-in badge beside the name while they fit, under it when not.
    LogosItemLayout {
        id: rowLayout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.spacing.medium
        anchors.rightMargin: Theme.spacing.medium
        spacing: Theme.spacing.medium

        leading: [
            LogosRadioButton {
                objectName: "appearance.radio." + root.themeId
                padding: 0
                spacing: 0
                // Shows the host's choice; a click only asks for it.
                checkable: false
                checked: root.current
                Accessible.name: root.name
                onClicked: if (!root.current) root.selectRequested()
            },
            Rectangle {
                id: swatch
                objectName: "appearance.swatch." + root.themeId
                implicitWidth: 36
                implicitHeight: 36
                radius: Theme.spacing.radiusSmall
                border.color: Theme.palette.borderSubtle
                border.width: Theme.size.borderWidth
                Accessible.ignored: true
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: root.background }
                    GradientStop { position: 0.5; color: root.background }
                    GradientStop { position: 0.5; color: root.accent }
                    GradientStop { position: 1.0; color: root.accent }
                }
            }
        ]

        ColumnLayout {
            spacing: 2

            LogosText {
                objectName: "appearance.name." + root.themeId
                Layout.fillWidth: true
                text: root.name
                font.pixelSize: Theme.typography.primaryText
                font.weight: Theme.typography.weightMedium
                color: Theme.palette.text
                elide: Text.ElideRight
            }
            LogosText {
                objectName: "appearance.author." + root.themeId
                Layout.fillWidth: true
                text: qsTr("by %1").arg(root.author)
                font.pixelSize: Theme.typography.secondaryText
                color: Theme.palette.textTertiary
                elide: Text.ElideRight
            }
        }

        meta: LogosBadge {
            objectName: "appearance.builtin." + root.themeId
            visible: root.builtin
            text: qsTr("Built-in")
            color: Theme.palette.textTertiary
        }

        trailing: LogosIconButton {
            id: removeButton
            objectName: "appearance.remove." + root.themeId
            visible: !root.builtin
            flat: true
            size: Theme.size.controlSmall
            iconSize: 18
            iconSource: LogosIcons.trash
            Accessible.name: qsTr("Remove %1").arg(root.name)
            onClicked: root.removeRequested()
            LogosToolTip {
                text: qsTr("Remove")
                placement: LogosToolTip.Top
                visible: removeButton.hovered
            }
        }
    }
}
