import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Layouts
import Logos.Theme

// Panel header shared by the Settings panels: the title, subtitle and row
// count, with Reload on their right, or under them when it doesn't fit. The
// search bar lives on the SettingsView page header, not inside the panel.
Item {
    id: root

    // ─── Public API ───
    property string title: ""
    property string subtitle: ""
    property alias reloadObjectName: reloadBtn.objectName
    property bool showReload: true
    property bool loading: false
    property int visibleCount: 0
    property int totalCount: 0

    signal reloadClicked()

    implicitHeight: layout.implicitHeight

    // Reload beside the texts while they keep their own width, under them
    // when not.
    LogosItemLayout {
        id: layout
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Theme.spacing.large
        minimumContentWidth: titleBlock.implicitWidth

        ColumnLayout {
            id: titleBlock
            spacing: Theme.spacing.tiny

            LogosText {
                Layout.fillWidth: true
                visible: root.title.length > 0
                text: root.title
                font.pixelSize: Theme.typography.panelTitleText
                font.weight: Theme.typography.weightMedium
                color: Theme.palette.text
                wrapMode: Text.Wrap
            }

            LogosText {
                visible: root.subtitle.length > 0
                Layout.fillWidth: true
                text: root.subtitle
                font.pixelSize: Theme.typography.secondaryText
                color: Theme.palette.textSecondary
                wrapMode: Text.WordWrap
            }

            LogosText {
                // Only interesting once loaded rows exist.
                visible: root.totalCount > 0
                Layout.fillWidth: true
                text: root.visibleCount === root.totalCount
                      ? qsTr("%n item(s)", "", root.totalCount)
                      : qsTr("%1 of %2").arg(root.visibleCount).arg(root.totalCount)
                font.pixelSize: Theme.typography.secondaryText
                color: Theme.palette.textTertiary
            }
        }

        meta: LogosButton {
            id: reloadBtn
            visible: root.showReload
            Layout.preferredWidth: 130
            Layout.preferredHeight: 40
            radius: Theme.spacing.radiusLarge
            text: qsTr("Reload")
            enabled: !root.loading
            leadingIcon.source: LogosIcons.refresh
            leadingIcon.size: 18
            onClicked: root.reloadClicked()
        }
    }
}
