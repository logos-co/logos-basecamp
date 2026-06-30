import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

Item {
    id: root

    signal installClicked()

    QtObject {
        id: d
        readonly property bool hasApps:
            typeof backend !== "undefined" && backend !== null &&
            (backend.launcherApps || []).length > 0
    }

    implicitWidth: 400
    implicitHeight: 300

    ColumnLayout {
        anchors.centerIn: parent
        spacing: Theme.spacing.medium

        LogosText {
            Layout.maximumWidth: root.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.typography.panelTitleText
            font.weight: Theme.typography.weightBold
            color: Theme.palette.text
            elide: Text.ElideRight
            text: d.hasApps ? qsTr("Welcome back")
                            : qsTr("Welcome to Basecamp!")
        }

        LogosText {
            Layout.maximumWidth: root.width
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.typography.primaryText
            color: Theme.palette.textSecondary
            elide: Text.ElideRight
            text: d.hasApps
                ? qsTr("Launch an installed app from the sidebar to open it here.")
                : qsTr("Browse the catalog and install your first app to get started.")
        }

        LogosButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spacing.medium
            Layout.preferredWidth: 200
            visible: !d.hasApps
            text: qsTr("Install now")
            onClicked: root.installClicked()
        }
    }
}
