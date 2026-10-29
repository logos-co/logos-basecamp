import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls

Item {
    id: root

    LogosScrollView {
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

                // "Version" stays a plain label so a drag-select copies the
                // bare version string, not the prefix.
                RowLayout {
                    Layout.fillWidth: true
                    visible: backend.buildVersion.length > 0
                    spacing: 6

                    LogosText {
                        text: "Version"
                        font.pixelSize: 18
                        color: "#ffffff"
                    }

                    LogosSelectableText {
                        Layout.fillWidth: true
                        text: backend.buildVersion
                        font.pixelSize: 18
                        color: "#ffffff"
                        wrapMode: TextEdit.WrapAnywhere
                    }
                }

                LogosText {
                    text: backend.isPortableBuild ? "Portable build" : "Dev build"
                    font.pixelSize: 14
                    color: "#a0a0a0"
                }
            }

            // Check for updates. Nothing goes to the network until it is pressed.
            RowLayout {
                id: updates
                readonly property var updater: backend.updater ?? null
                readonly property string phase: updater ? updater.state : ""
                visible: updater !== null
                Layout.fillWidth: true
                spacing: 12

                LogosButton {
                    objectName: "dashboard.updateButton"
                    implicitHeight: 36
                    visible: updates.phase !== "ready" || updates.updater.canRestart
                    enabled: updates.phase !== "checking" && updates.phase !== "downloading"
                    text: {
                        switch (updates.phase) {
                        case "checking": return qsTr("Checking…")
                        case "available": return qsTr("Update to %1").arg(updates.updater.latestVersion)
                        case "downloading": return qsTr("Downloading… %1%").arg(Math.round(updates.updater.progress * 100))
                        case "ready": return qsTr("Restart to update")
                        default: return qsTr("Check for updates")
                        }
                    }
                    onClicked: {
                        if (updates.phase === "available") updates.updater.update()
                        else if (updates.phase === "ready") updates.updater.restart()
                        else updates.updater.check()
                    }
                }

                LogosText {
                    objectName: "dashboard.updateStatus"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 14
                    color: updates.phase === "error" ? "#ff8080" : "#a0a0a0"
                    text: updates.phase === "upToDate" ? qsTr("Basecamp is up to date.")
                        : updates.updater && updates.updater.message.length > 0 ? updates.updater.message
                        : updates.phase === "available" ? qsTr("Version %1 is available.").arg(updates.updater.latestVersion)
                        : ""
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

                        // Module name and commit hash are both copy targets, so
                        // they wrap instead of eliding — an elided hash cannot
                        // be selected in full.
                        LogosSelectableText {
                            text: modelData.name
                            color: "#a0a0a0"
                            font.pixelSize: 13
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignTop
                            wrapMode: TextEdit.WrapAnywhere
                        }
                        LogosSelectableText {
                            text: modelData.commit
                            color: "#d0d0d0"
                            font.family: "monospace"
                            font.pixelSize: 13
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            wrapMode: TextEdit.WrapAnywhere
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
