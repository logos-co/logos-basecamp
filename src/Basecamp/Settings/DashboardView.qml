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
                            text: modelData.name
                            color: Theme.palette.textSecondary
                            font.pixelSize: Theme.typography.primaryText
                            wrapMode: TextEdit.WrapAnywhere
                        }

                        meta: LogosSelectableText {
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
