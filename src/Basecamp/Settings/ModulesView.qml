import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls

Item {
    id: root

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 20


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



