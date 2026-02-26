import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls

Item {
    id: root

    ColumnLayout {
        anchors.fill: parent
        spacing: 20

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                LogosText {
                    text: "UI Modules"
                    font.pixelSize: 20
                    font.weight: Font.Bold
                    color: "#ffffff"
                }

                LogosText {
                    text: "Available UI plugins in the system"
                    color: "#a0a0a0"
                }
            }

        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ColumnLayout {
                width: parent.width
                spacing: 8

                Repeater {
                    model: backend.uiModules

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 70
                        color: "#2d2d2d"
                        radius: 8
                        border.color: "#3d3d3d"
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 12

                            // Icon
                            Rectangle {
                                width: 48
                                height: 48
                                radius: 8
                                color: "#3d3d3d"

                                Image {
                                    anchors.centerIn: parent
                                    source: modelData.iconPath || ""
                                    sourceSize.width: 32
                                    sourceSize.height: 32
                                    visible: modelData.iconPath && modelData.iconPath.length > 0
                                }

                                LogosText {
                                    anchors.centerIn: parent
                                    text: modelData.name.substring(0, 2).toUpperCase()
                                    font.pixelSize: 16
                                    font.weight: Font.Bold
                                    color: "#808080"
                                    visible: !modelData.iconPath || modelData.iconPath.length === 0
                                }
                            }

                            // Name
                            LogosText {
                                text: modelData.name
                                font.pixelSize: 16
                                font.weight: Font.Bold
                                color: "#ffffff"
                                Layout.fillWidth: true
                            }

                            // Load/Unload buttons (hidden for main_ui)
                            Button {
                                text: "Load"
                                visible: !modelData.isMainUi && !modelData.isLoaded

                                contentItem: LogosText {
                                    text: parent.text
                                    font.pixelSize: 13
                                    color: "#ffffff"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                background: Rectangle {
                                    implicitWidth: 80
                                    implicitHeight: 32
                                    color: parent.pressed ? "#45a049" : "#4CAF50"
                                    radius: 4
                                }

                                onClicked: backend.loadUiModule(modelData.name)
                            }

                            Button {
                                text: "Unload"
                                visible: !modelData.isMainUi && modelData.isLoaded

                                contentItem: LogosText {
                                    text: parent.text
                                    font.pixelSize: 13
                                    color: "#ffffff"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                background: Rectangle {
                                    implicitWidth: 80
                                    implicitHeight: 32
                                    color: parent.pressed ? "#da190b" : "#f44336"
                                    radius: 4
                                }

                                onClicked: backend.unloadUiModule(modelData.name)
                            }
                        }
                    }
                }

                // Empty state
                LogosText {
                    text: "No UI plugins found in the plugins directory."
                    color: "#606060"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 40
                    visible: backend.uiModules.length === 0
                }
            }
        }
    }
}



