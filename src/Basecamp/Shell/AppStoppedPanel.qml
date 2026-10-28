import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

Rectangle {
    id: root
    objectName: "appStopped.root"

    property string appId: ""     // module name, carried by the signals
    property string appName: ""   // display name
    property bool closable: true  // false for an embedded page (Package Manager)

    signal reopenRequested(string appId)
    signal closeRequested(string appId)

    color: Theme.palette.background

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.spacing.xxlarge, 420)
        spacing: Theme.spacing.medium

        LogosText {
            objectName: "appStopped.title"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText  // the name comes from the app's package
            text: qsTr("%1 stopped unexpectedly").arg(root.appName)
            font.pixelSize: Theme.typography.subtitleText
            font.weight: Theme.typography.weightBold
            color: Theme.palette.text
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: false
            Layout.topMargin: Theme.spacing.small
            spacing: Theme.spacing.small

            LogosButton {
                objectName: "appStopped.close"
                visible: root.closable
                text: qsTr("Close")
                onClicked: root.closeRequested(root.appId)
            }
            LogosButton {
                objectName: "appStopped.reopen"
                text: qsTr("Reopen")
                variant: LogosButton.Variant.Primary
                onClicked: root.reopenRequested(root.appId)
            }
        }
    }
}
