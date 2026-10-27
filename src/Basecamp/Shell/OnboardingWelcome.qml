import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

// First-run landing: the backdrop, the greeting and the two ways in.
Item {
    id: root

    property url backdrop
    property url logo

    signal quickSetupRequested()
    signal customSetupRequested()

    Image {
        anchors.fill: parent
        source: root.backdrop
        fillMode: Image.PreserveAspectCrop
        mipmap: true
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.palette.scrim
    }

    ColumnLayout {
        objectName: "onboardingWelcomeColumn"
        anchors.centerIn: parent
        width: Math.min(root.width - 96, 520)
        spacing: Theme.spacing.medium

        Image {
            objectName: "onboardingLogo"
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: Theme.spacing.medium
            source: root.logo
            sourceSize.height: 48
            fillMode: Image.PreserveAspectFit
        }

        LogosText {
            objectName: "onboardingGreeting"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Welcome to Basecamp,")
            color: Theme.palette.text
            font.pixelSize: Theme.typography.pageTitleText
            font.weight: Theme.typography.weightBold
            wrapMode: Text.WordWrap
        }

        LogosText {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Let's get you set up, it'll only take a moment.")
            color: Theme.palette.text
            font.pixelSize: Theme.typography.subtitleText
            wrapMode: Text.WordWrap
        }

        ColumnLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spacing.large
            Layout.preferredWidth: 320
            spacing: Theme.spacing.medium
            LogosButton {
                objectName: "onboardingQuickSetupButton"
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                variant: LogosButton.Variant.Primary
                font.pixelSize: Theme.typography.primaryText
                font.weight: Theme.typography.weightMedium
                text: qsTr("Let's go")
                onClicked: root.quickSetupRequested()
            }

            LogosButton {
                objectName: "onboardingCustomSetupButton"
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                font.pixelSize: Theme.typography.primaryText
                font.weight: Theme.typography.weightMedium
                text: qsTr("Advanced setup")
                onClicked: root.customSetupRequested()

                background: Rectangle {
                    readonly property var control: parent

                    radius: control.radius
                    color: control.pressed  ? Theme.palette.background
                         : control.hovered  ? Theme.palette.backgroundTertiary
                                            : Theme.palette.backgroundSecondary
                    border.width: 1
                    border.color: Theme.palette.border
                }
            }
        }
    }
}
