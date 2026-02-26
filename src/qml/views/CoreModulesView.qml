import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls

Item {
    id: root

    property string selectedPlugin: ""
    property bool showingMethods: false

    onVisibleChanged: {
        if (visible) {
            backend.refreshCoreModules();
        }
    }

    Component.onCompleted: {
        backend.refreshCoreModules();
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: root.showingMethods ? 1 : 0

        // Plugin list view
        ColumnLayout {
            spacing: 20

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    LogosText {
                        text: "Core Modules"
                        font.pixelSize: 20
                        font.weight: Font.Bold
                        color: "#ffffff"
                    }

                    LogosText {
                        text: "All available plugins in the system"
                        color: "#a0a0a0"
                    }
                }

                Button {
                    text: "Reload"
                    onClicked: backend.refreshCoreModules()

                    contentItem: LogosText {
                        text: parent.text
                        font.pixelSize: 13
                        color: "#ffffff"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        implicitWidth: 100
                        implicitHeight: 32
                        color: parent.pressed ? "#3d3d3d" : "#4d4d4d"
                        radius: 4
                        border.color: "#5d5d5d"
                        border.width: 1
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#2d2d2d"
                radius: 8
                border.color: "#3d3d3d"
                border.width: 1

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 20
                    clip: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 8

                        Repeater {
                            model: backend.coreModules

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 50
                                color: index % 2 === 0 ? "#363636" : "#2d2d2d"
                                radius: 6

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 10

                                    // Plugin name
                                    LogosText {
                                        text: modelData.name
                                        font.pixelSize: 16
                                        color: "#e0e0e0"
                                        Layout.preferredWidth: 150
                                    }

                                    // Status
                                    LogosText {
                                        text: modelData.isLoaded ? "(Loaded)" : "(Not Loaded)"
                                        color: modelData.isLoaded ? "#4CAF50" : "#F44336"
                                    }

                                    // CPU (only for loaded)
                                    LogosText {
                                        text: modelData.isLoaded ? "CPU: " + modelData.cpu + "%" : ""
                                        color: "#64B5F6"
                                        Layout.preferredWidth: 80
                                    }

                                    // Memory (only for loaded)
                                    LogosText {
                                        text: modelData.isLoaded ? "Mem: " + modelData.memory + " MB" : ""
                                        color: "#81C784"
                                        Layout.preferredWidth: 100
                                    }

                                    Item { Layout.fillWidth: true }

                                    // Load/Unload button
                                    Button {
                                        text: modelData.isLoaded ? "Unload Plugin" : "Load Plugin"
                                        
                                        contentItem: LogosText {
                                            text: parent.text
                                            font.pixelSize: 12
                                            color: "#ffffff"
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }

                                        background: Rectangle {
                                            implicitWidth: 100
                                            implicitHeight: 30
                                            color: modelData.isLoaded ? 
                                                (parent.pressed ? "#da190b" : "#F44336") :
                                                (parent.pressed ? "#3d8b40" : "#4b4b4b")
                                            radius: 4
                                        }

                                        onClicked: {
                                            if (modelData.isLoaded) {
                                                backend.unloadCoreModule(modelData.name)
                                            } else {
                                                backend.loadCoreModule(modelData.name)
                                            }
                                        }
                                    }

                                    // View Methods button (only for loaded)
                                    Button {
                                        text: "View Methods"
                                        visible: modelData.isLoaded
                                        
                                        contentItem: LogosText {
                                            text: parent.text
                                            font.pixelSize: 12
                                            color: "#ffffff"
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }

                                        background: Rectangle {
                                            implicitWidth: 100
                                            implicitHeight: 30
                                            color: parent.pressed ? "#3d3d3d" : "#4b4b4b"
                                            radius: 4
                                        }

                                        onClicked: {
                                            root.selectedPlugin = modelData.name
                                            root.showingMethods = true
                                        }
                                    }
                                }
                            }
                        }

                        // Empty state
                        LogosText {
                            text: "No core modules available."
                            color: "#606060"
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 40
                            visible: backend.coreModules.length === 0
                        }
                    }
                }
            }
        }

        // Methods view
        PluginMethodsView {
            pluginName: root.selectedPlugin
            onBackClicked: root.showingMethods = false
        }
    }
}



