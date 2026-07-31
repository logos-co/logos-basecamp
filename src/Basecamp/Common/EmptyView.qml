import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

// Centered empty-state placeholder
// Example:
//     EmptyView {
//         Layout.fillWidth: true
//         Layout.fillHeight: true
//         visible: repositories.length === 0
//         title:      qsTr("No repositories configured")
//         subtitle:   qsTr("Add a repository to browse and install apps.")
//         actionText: qsTr("Manage Repositories")
//         onActionClicked: navigateToRepositories()
//     }
Item {
    id: root

    property string title: ""
    property string subtitle: ""
    property string actionText: ""

    signal actionClicked()

    ColumnLayout {
        anchors.centerIn: parent
        spacing: Theme.spacing.medium
        // Cap width so long subtitles wrap sensibly in wide panels while
        // narrow panels shrink to fit.
        width: Math.min(parent.width * 0.6, 420)

        LogosText {
            Layout.alignment: Qt.AlignHCenter
            visible: root.title.length > 0
            text: root.title
            font.pixelSize: Theme.typography.subtitleText
            font.weight: Theme.typography.weightMedium
            color: Theme.palette.text
            horizontalAlignment: Text.AlignHCenter
        }

        LogosText {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            visible: root.subtitle.length > 0
            text: root.subtitle
            font.pixelSize: Theme.typography.primaryText
            color: Theme.palette.textSecondary
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        LogosButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spacing.small
            visible: root.actionText.length > 0
            text: root.actionText
            onClicked: root.actionClicked()
        }
    }
}
