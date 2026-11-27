import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: root
    objectName: "pluginInterfaceView"

    property string pluginName: ""
    property var methods: []
    property var events: []
    property string resultText: ""

    Component.onCompleted: { loadMethods(); loadEvents() }
    onPluginNameChanged: { loadMethods(); loadEvents() }

    function loadMethods() {
        if (pluginName.length > 0) {
            let methodsJson = backend.getCoreModuleMethods(pluginName)
            try {
                methods = JSON.parse(methodsJson)
            } catch (e) {
                methods = []
            }
        } else {
            // Cleared/reset: don't leave a previous plugin's data on screen.
            methods = []
            resultText = ""
        }
    }

    function loadEvents() {
        if (pluginName.length > 0) {
            let eventsJson = backend.getCoreModuleEvents(pluginName)
            try {
                events = JSON.parse(eventsJson)
            } catch (e) {
                events = []
            }
        } else {
            events = []
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing.xlarge

        // Its way back is Settings' back row, over it.
        LogosText {
            Layout.fillWidth: true
            text: "Interface: " + root.pluginName
            font.pixelSize: Theme.typography.panelTitleText
            font.weight: Theme.typography.weightBold
            color: Theme.palette.text
            elide: Text.ElideRight
        }

        // Methods list
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.palette.backgroundSecondary
            radius: Theme.spacing.radiusLarge
            border.color: Theme.palette.border
            border.width: Theme.size.borderWidth

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacing.xlarge
                spacing: Theme.spacing.large

                LogosScrollView {
                    id: interfaceScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    ColumnLayout {
                        // Bind to the ScrollView's `availableWidth`, not
                        // `parent.width`: inside a Qt 6 ScrollView the latter
                        // resolves against the internal Flickable's contentItem,
                        // which can transiently collapse to 0 while a Repeater
                        // recreates delegates — snapping every `fillWidth` child
                        // to width 0 without always recovering. `availableWidth`
                        // is the stable public property (scrollbars included).
                        width: interfaceScroll.availableWidth
                        spacing: Theme.spacing.small

                        // ── Methods ──
                        LogosText {
                            text: "Methods"
                            font.pixelSize: Theme.typography.secondaryText
                            font.weight: Theme.typography.weightBold
                            color: Theme.palette.textSecondary
                            visible: root.methods.length > 0
                        }

                        Repeater {
                            model: root.methods

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: methodColumn.implicitHeight + 2 * Theme.spacing.medium
                                color: Theme.palette.surface
                                radius: Theme.spacing.radiusMedium

                                ColumnLayout {
                                    id: methodColumn
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacing.medium
                                    spacing: Theme.spacing.small

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacing.medium

                                        // The signature beside the name while
                                        // it fits, under it when it doesn't.
                                        Flow {
                                            id: methodTitle
                                            Layout.fillWidth: true
                                            spacing: Theme.spacing.medium

                                            LogosText {
                                                width: Math.min(implicitWidth, methodTitle.width)
                                                text: modelData.name || modelData
                                                font.weight: Theme.typography.weightBold
                                                color: Theme.palette.info
                                                elide: Text.ElideRight
                                            }

                                            LogosText {
                                                width: Math.min(implicitWidth, methodTitle.width)
                                                text: modelData.signature || ""
                                                font.pixelSize: Theme.typography.secondaryText
                                                // textTertiary is under 4.5:1 on the surface card.
                                                color: Theme.palette.textSecondary
                                                wrapMode: Text.WrapAnywhere
                                                visible: modelData.signature !== undefined
                                            }
                                        }

                                        LogosButton {
                                            objectName: "pluginInterface.call." + (modelData.name || modelData)
                                            text: "Call"
                                            compact: true
                                            variant: LogosButton.Variant.Primary

                                            onClicked: {
                                                let methodName = modelData.name || modelData
                                                root.resultText = backend.callCoreModuleMethod(root.pluginName, methodName, "[]")
                                            }
                                        }
                                    }

                                    LogosText {
                                        text: modelData.description || ""
                                        font.pixelSize: Theme.typography.secondaryText
                                        color: Theme.palette.textSecondary
                                        wrapMode: Text.Wrap
                                        Layout.fillWidth: true
                                        visible: modelData.description !== undefined && modelData.description.length > 0
                                    }
                                }
                            }
                        }

                        // Empty state
                        LogosText {
                            text: "No methods available for this plugin."
                            color: Theme.palette.textTertiary
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            Layout.topMargin: Theme.spacing.xxlarge
                            visible: root.methods.length === 0
                        }

                        // ── Events ──
                        LogosText {
                            text: "Events"
                            font.pixelSize: Theme.typography.secondaryText
                            font.weight: Theme.typography.weightBold
                            color: Theme.palette.textSecondary
                            Layout.topMargin: Theme.spacing.medium
                            visible: root.events.length > 0
                        }

                        Repeater {
                            model: root.events

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: eventColumn.implicitHeight + 2 * Theme.spacing.medium
                                color: Theme.palette.surface
                                radius: Theme.spacing.radiusMedium

                                ColumnLayout {
                                    id: eventColumn
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacing.medium
                                    spacing: Theme.spacing.small

                                    // As a method's: beside the name, or under it.
                                    Flow {
                                        id: eventTitle
                                        Layout.fillWidth: true
                                        spacing: Theme.spacing.medium

                                        LogosText {
                                            width: Math.min(implicitWidth, eventTitle.width)
                                            text: modelData.name || modelData
                                            font.weight: Theme.typography.weightBold
                                            // Amber to distinguish events from the blue methods.
                                            color: Theme.palette.warning
                                            elide: Text.ElideRight
                                        }

                                        LogosText {
                                            width: Math.min(implicitWidth, eventTitle.width)
                                            text: modelData.signature || ""
                                            font.pixelSize: Theme.typography.secondaryText
                                            // textTertiary is under 4.5:1 on the surface card.
                                            color: Theme.palette.textSecondary
                                            wrapMode: Text.WrapAnywhere
                                            visible: modelData.signature !== undefined
                                        }
                                    }

                                    LogosText {
                                        text: modelData.description || ""
                                        font.pixelSize: Theme.typography.secondaryText
                                        color: Theme.palette.textSecondary
                                        wrapMode: Text.Wrap
                                        Layout.fillWidth: true
                                        visible: modelData.description !== undefined && modelData.description.length > 0
                                    }
                                }
                            }
                        }
                    }
                }

                // Result area
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 150
                    color: Theme.palette.surfaceRecessed
                    radius: Theme.spacing.radiusSmall
                    border.color: Theme.palette.border
                    border.width: Theme.size.borderWidth
                    visible: root.resultText.length > 0

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.small

                        LogosText {
                            text: "Result:"
                            font.pixelSize: Theme.typography.secondaryText
                            font.weight: Theme.typography.weightBold
                            color: Theme.palette.textSecondary
                        }

                        LogosScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true

                            TextArea {
                                objectName: "pluginInterface.result"
                                text: root.resultText
                                font.pixelSize: Theme.typography.secondaryText
                                font.family: Theme.typography.publicSans
                                color: Theme.palette.success
                                readOnly: true
                                wrapMode: Text.Wrap
                                background: Rectangle {
                                    color: "transparent"
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}



