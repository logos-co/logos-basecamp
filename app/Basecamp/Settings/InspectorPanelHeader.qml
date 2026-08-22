import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Theme

// Panel header shared by the two Settings inspectors. Same shape as
// AppManagerPanelHeader: title block on the left, Reload plus the row count on
// the right. The search bar lives on the SettingsView page header (right of the
// page title, matching AppManagerView) — not inside the panel container.
Item {
    id: root

    // ─── Public API ───
    property string title: ""
    property string subtitle: ""
    property alias reloadObjectName: reloadBtn.objectName
    property bool loading: false
    property int visibleCount: 0
    property int totalCount: 0

    signal reloadClicked()

    implicitHeight: layout.implicitHeight

    GridLayout {
        id: layout
        anchors.left: parent.left
        anchors.right: parent.right
        columnSpacing: Theme.spacing.large
        rowSpacing: Theme.spacing.small
        // Two columns while title + controls fit side by side, one when the
        // pane gets narrow — same responsive rule as AppManagerPanelHeader.
        columns: (titleBlock.implicitWidth + controls.implicitWidth + columnSpacing) <= width
                 ? 2 : 1

        ColumnLayout {
            id: titleBlock
            Layout.fillWidth: true
            spacing: Theme.spacing.tiny

            LogosText {
                text: root.title
                font.pixelSize: Theme.typography.panelTitleText
                font.weight: Theme.typography.weightMedium
                color: Theme.palette.text
            }

            LogosText {
                visible: root.subtitle.length > 0
                Layout.fillWidth: true
                text: root.subtitle
                font.pixelSize: Theme.typography.secondaryText
                color: Theme.palette.textSecondary
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            id: controls
            Layout.fillWidth: true
            spacing: Theme.spacing.medium

            Item { Layout.fillWidth: layout.columns === 2 }

            LogosText {
                // Only interesting once loaded rows exist.
                visible: root.totalCount > 0
                text: root.visibleCount === root.totalCount
                      ? qsTr("%n item(s)", "", root.totalCount)
                      : qsTr("%1 of %2").arg(root.visibleCount).arg(root.totalCount)
                font.pixelSize: Theme.typography.secondaryText
                color: Theme.palette.textTertiary
            }

            LogosButton {
                id: reloadBtn
                Layout.minimumWidth: 80
                Layout.preferredWidth: 130
                Layout.maximumWidth: 130
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
}
