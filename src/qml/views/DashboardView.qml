import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls

Item {
    id: root

    Rectangle {
        anchors.fill: parent
        color: "#1e1e1e"
    }

    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: 40
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 20

            LogosText {
                text: "Dashboard"
                font.pixelSize: 24
                font.weight: Font.Bold
                color: "#ffffff"
            }

            // Build summary: version (release only) + build type.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                LogosText {
                    visible: backend.buildVersion.length > 0
                    text: "Version " + backend.buildVersion
                    font.pixelSize: 18
                    color: "#ffffff"
                }

                LogosText {
                    text: backend.isPortableBuild ? "Portable build" : "Dev build"
                    font.pixelSize: 14
                    color: "#a0a0a0"
                }
            }

            // Commit hashes for basecamp + all flake dependencies.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                spacing: 4

                LogosText {
                    text: "Commits"
                    font.pixelSize: 16
                    font.weight: Font.Bold
                    color: "#ffffff"
                }

                Repeater {
                    model: backend.buildCommits
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        LogosText {
                            text: modelData.name
                            color: "#a0a0a0"
                            font.pixelSize: 13
                            Layout.preferredWidth: 260
                            elide: Text.ElideRight
                        }
                        LogosText {
                            text: modelData.commit
                            color: "#d0d0d0"
                            font.family: "monospace"
                            font.pixelSize: 13
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
