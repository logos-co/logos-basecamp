pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

// One setup step: the title, the step rail and the footer around its content.
// `objectPrefix` names the controls, unique per step since every step has its
// own.
Item {
    id: root

    property string objectPrefix: "onboarding"
    property var stepNames: []
    property int stepIndex: 0
    property bool canGoBack: false
    property bool canAdvance: true
    property string advanceText: ""
    property string advanceHint: ""
    // A second way forward beside the advance button, e.g. Retry. Hidden when empty.
    property string secondaryText: ""

    // The step itself, filling the space between the rail and the footer.
    default property alias content: contentArea.data

    signal backRequested()
    signal advanceRequested()
    signal secondaryRequested()

    ColumnLayout {
        anchors.fill: parent
        // The page is the whole window, not a view inside Basecamp's chrome.
        anchors.margins: Theme.spacing.xxlarge
        spacing: Theme.spacing.large

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            LogosText {
                text: qsTr("Set up Basecamp")
                color: Theme.palette.text
                font.pixelSize: Theme.typography.titleText
                font.weight: Theme.typography.weightBold
            }

            LogosText {
                text: qsTr("Advanced setup: choose how Basecamp downloads apps.")
                color: Theme.palette.textSecondary
                font.pixelSize: Theme.typography.secondaryText
            }
        }

        RowLayout {
            objectName: root.objectPrefix + "StepRail"
            visible: root.stepNames.length > 1
            Layout.fillWidth: true
            spacing: Theme.spacing.small

            Repeater {
                model: root.stepNames

                delegate: ColumnLayout {
                    id: railStep
                    required property int index
                    required property string modelData

                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    spacing: 4

                    Rectangle {
                        Layout.fillWidth: true
                        height: 4
                        radius: 2
                        color: railStep.index <= root.stepIndex ? Theme.palette.primary
                                                                : Theme.palette.border
                    }

                    LogosText {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: (railStep.index + 1) + ". " + railStep.modelData
                        color: railStep.index === root.stepIndex ? Theme.palette.text
                                                                 : Theme.palette.textTertiary
                        font.pixelSize: 11
                        font.weight: railStep.index === root.stepIndex
                            ? Theme.typography.weightBold
                            : Theme.typography.weightRegular
                    }
                }
            }
        }

        LogosScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth

            Item {
                id: contentArea
                width: scroll.availableWidth
                height: Math.max(scroll.availableHeight,
                                 children.length > 0 ? children[0].implicitHeight : 0)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.medium

            LogosButton {
                objectName: root.objectPrefix + "BackButton"
                visible: root.canGoBack
                text: qsTr("Back")
                onClicked: root.backRequested()
            }

            Item { Layout.fillWidth: true }

            LogosText {
                objectName: root.objectPrefix + "AdvanceHint"
                visible: root.advanceHint.length > 0
                text: root.advanceHint
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
            }

            LogosButton {
                objectName: root.objectPrefix + "SecondaryButton"
                visible: root.secondaryText.length > 0
                Layout.preferredHeight: 44
                text: root.secondaryText
                onClicked: root.secondaryRequested()
            }

            LogosButton {
                objectName: root.objectPrefix + "AdvanceButton"
                variant: LogosButton.Variant.Primary
                Layout.preferredHeight: 44
                Layout.minimumWidth: 160
                enabled: root.canAdvance
                text: root.advanceText
                onClicked: root.advanceRequested()
            }
        }
    }
}
