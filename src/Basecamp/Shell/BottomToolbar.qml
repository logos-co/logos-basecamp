import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Theme

Rectangle {
    id: root

    implicitHeight: 50
    color: Theme.palette.backgroundTertiary
    radius: Theme.spacing.radiusXlarge

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spacing.medium
        anchors.rightMargin: Theme.spacing.medium
        spacing: Theme.spacing.medium

        Item { Layout.fillWidth: true }

        Text {
            text: backend.buildVersion
            color: Theme.palette.textSecondary
            font.pixelSize: 12
            elide: Text.ElideRight
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
        }
    }
}
