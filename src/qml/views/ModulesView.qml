import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import panels

Item {
    id: root

    Rectangle {
        anchors.fill: parent
        color: "#1e1e1e"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 20

        RowLayout {
            Layout.fillWidth: true

            LogosText {
                text: "Modules"
                font.pixelSize: 24
                font.weight: Font.Bold
                color: "#ffffff"
                Layout.fillWidth: true
            }

            Button {
                text: "Install LGX Package"
                onClicked: backend.openInstallPluginDialog()

                contentItem: LogosText {
                    text: parent.text
                    font.pixelSize: 13
                    color: "#ffffff"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    implicitWidth: 180
                    implicitHeight: 32
                    color: parent.pressed ? "#1a7f37" : "#238636"
                    radius: 4
                    border.color: "#2ea043"
                    border.width: 1
                }
            }
        }

        TabBar {
            id: tabBar
            Layout.fillWidth: true
            
            background: Rectangle {
                color: "transparent"
            }

            TabButton {
                text: "UI Modules"
                width: implicitWidth + 32
                
                contentItem: LogosText {
                    text: parent.text
                    color: tabBar.currentIndex === 0 ? "#ffffff" : "#cccccc"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                
                background: Rectangle {
                    color: tabBar.currentIndex === 0 ? "#2d2d2d" : "#1d1d1d"
                    radius: 5
                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 5
                        color: parent.color
                    }
                }
            }

            TabButton {
                text: "Core Modules"
                width: implicitWidth + 32
                
                contentItem: LogosText {
                    text: parent.text
                    color: tabBar.currentIndex === 1 ? "#ffffff" : "#cccccc"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                
                background: Rectangle {
                    color: tabBar.currentIndex === 1 ? "#2d2d2d" : "#1d1d1d"
                    radius: 5
                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 5
                        color: parent.color
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex
            
            onCurrentIndexChanged: {
                if (currentIndex === 1) {
                    backend.refreshCoreModules();
                }
            }

            // UI Modules tab
            UiModulesTab {
                id: uiModulesTab
            }

            // Core Modules tab
            CoreModulesView {
                id: coreModulesTab
            }
        }
    }
}



