import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Layouts
import Logos.Theme

Item {
    id: root

    property string buildVersion: ""
    property bool portableBuild: false
    property var buildCommits: []   // [{ name, commit }]
    property string userDirectory: ""

    signal openUserDirectoryRequested()

    function reportOpenUserDirectoryFailed() { d.openFailed = true }

    QtObject {
        id: d
        property bool openFailed: false
    }

    LogosScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: Theme.pageMargin(root.width)
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: Theme.spacing.xlarge

            LogosText {
                Layout.fillWidth: true
                text: qsTr("Dashboard")
                font.pixelSize: Theme.typography.panelTitleText
                font.weight: Theme.typography.weightBold
                color: Theme.palette.text
                wrapMode: Text.Wrap
            }

            // Build summary: version (release only) + build type.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.small

                // "Version" stays a plain label so a drag-select copies the
                // bare version string, not the prefix.
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.buildVersion.length > 0
                    spacing: Theme.spacing.small

                    LogosText {
                        Layout.alignment: Qt.AlignTop
                        text: qsTr("Version")
                        font.pixelSize: Theme.typography.subtitleText
                        color: Theme.palette.text
                    }

                    LogosSelectableText {
                        Layout.fillWidth: true
                        text: root.buildVersion
                        font.pixelSize: Theme.typography.subtitleText
                        color: Theme.palette.text
                        wrapMode: TextEdit.WrapAnywhere
                    }
                }

                LogosText {
                    text: root.portableBuild ? qsTr("Portable build") : qsTr("Dev build")
                    font.pixelSize: Theme.typography.primaryText
                    color: Theme.palette.textSecondary
                }
            }

            // Where this instance keeps its data (logs, modules, module_data):
            // the default or --user-dir.
            LogosFrame {
                Layout.fillWidth: true
                visible: root.userDirectory.length > 0

                // Its content is a column: title, path, then any failure.
                LogosItemLayout {
                    width: parent.width

                    LogosText {
                        text: qsTr("User directory")
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.text
                        wrapMode: Text.Wrap
                    }

                    LogosSelectableText {
                        objectName: "dashboard.userDirectory"
                        Layout.topMargin: Theme.spacing.tiny
                        text: root.userDirectory
                        color: Theme.palette.textSecondary
                        font.family: Theme.typography.mono
                        font.pixelSize: Theme.typography.primaryText
                        wrapMode: TextEdit.WrapAnywhere
                    }

                    LogosText {
                        objectName: "dashboard.openUserDirectoryFailed"
                        Layout.topMargin: Theme.spacing.small
                        visible: d.openFailed
                        text: qsTr("Couldn't open the folder. Copy its path instead.")
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.error
                        wrapMode: Text.Wrap
                    }

                    trailing: LogosButton {
                        objectName: "dashboard.openUserDirectory"
                        text: Qt.platform.os === "osx" ? qsTr("Show in Finder")
                            : Qt.platform.os === "windows" ? qsTr("Show in Explorer")
                            : qsTr("Open folder")
                        onClicked: {
                            d.openFailed = false
                            root.openUserDirectoryRequested()
                        }
                    }
                }
            }

            // Commit hashes for basecamp + all flake dependencies.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.small

                LogosText {
                    text: qsTr("Commits")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }

                Repeater {
                    model: root.buildCommits
                    delegate: LogosItemLayout {
                        Layout.fillWidth: true

                        LogosSelectableText {
                            objectName: "dashboard.commitName"
                            text: modelData.name
                            color: Theme.palette.textSecondary
                            font.pixelSize: Theme.typography.primaryText
                            wrapMode: TextEdit.WrapAnywhere
                        }

                        meta: LogosSelectableText {
                            objectName: "dashboard.commitHash"
                            Layout.fillWidth: true
                            text: modelData.commit
                            color: Theme.palette.text
                            font.family: Theme.typography.mono
                            font.pixelSize: Theme.typography.primaryText
                            wrapMode: TextEdit.WrapAnywhere
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
