import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Theme

Item {
    id: root

    property var repositories: []
    property bool loading: false

    signal refreshRequested()
    signal addRequested(string url)
    signal removeRequested(string url)
    signal setEnabledRequested(string url, bool enabled)

    function reportOperationResult(operation, url, success, error) {
        if (success) {
            d.lastError = ""
            if (operation === "add") d.newRepoUrl = ""
            return
        }
        d.lastError = error.length > 0 ? error : qsTr("Operation failed")
    }

    QtObject {
        id: d
        property string newRepoUrl: ""
        property string lastError: ""
    }

    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: Theme.spacing.large
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: Theme.spacing.large

            // Header.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.medium

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.tiny

                    LogosText {
                        text: qsTr("Package Repositories")
                        font.pixelSize: Theme.typography.panelTitleText
                        font.weight: Theme.typography.weightMedium
                        color: Theme.palette.text
                    }
                    LogosText {
                        text: qsTr("The catalog merges the built-in repository with any repositories you add here.")
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textSecondary
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                LogosButton {
                    text: root.loading ? qsTr("Refreshing…") : qsTr("Refresh")
                    enabled: !root.loading
                    implicitWidth: 120
                    implicitHeight: 36
                    onClicked: root.refreshRequested()
                }
            }

            // Error banner — shown until the next successful op.
            Rectangle {
                Layout.fillWidth: true
                visible: d.lastError.length > 0
                radius: Theme.spacing.radiusSmall
                color: Theme.colors.getColor(Theme.palette.error, 0.12)
                border.color: Theme.palette.error
                border.width: 1
                implicitHeight: errorRow.implicitHeight + Theme.spacing.medium * 2

                RowLayout {
                    id: errorRow
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.medium
                    spacing: Theme.spacing.medium

                    LogosText {
                        Layout.fillWidth: true
                        text: d.lastError
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.secondaryText
                        wrapMode: Text.WordWrap
                    }
                    LogosIconButton {
                        iconSource: LogosIcons.close
                        size: 20
                        iconSize: 14
                        iconColor: Theme.palette.textTertiary
                        background: Item {}
                        onClicked: d.lastError = ""
                    }
                }
            }

            // Repository list.
            Repeater {
                model: root.repositories
                delegate: Rectangle {
                    required property var modelData

                    readonly property string url:          modelData.url || ""
                    readonly property string displayName:  modelData.displayName || modelData.name || ""
                    readonly property string description:  modelData.description || ""
                    readonly property string resolveError: modelData.resolveError || ""
                    readonly property bool   isDefault:    modelData.isDefault === true
                    readonly property bool   isEnabled:    modelData.enabled !== false

                    Layout.fillWidth: true
                    implicitHeight: rowCol.implicitHeight + Theme.spacing.large * 2
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.background
                    border.color: Theme.palette.borderSubtle
                    border.width: 1

                    ColumnLayout {
                        id: rowCol
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.large
                        spacing: Theme.spacing.small

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.small

                            LogosText {
                                text: displayName.length > 0 ? displayName : qsTr("(unnamed repository)")
                                font.pixelSize: Theme.typography.primaryText
                                font.weight: Theme.typography.weightMedium
                                color: Theme.palette.text
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            LogosBadge {
                                visible: isDefault
                                text: qsTr("Default")
                                color: Theme.palette.accentOrange
                            }
                            LogosBadge {
                                visible: !isEnabled
                                text: qsTr("Disabled")
                                color: Theme.palette.textTertiary
                            }
                            LogosBadge {
                                visible: resolveError.length > 0
                                text: qsTr("Unreachable")
                                color: Theme.palette.error
                            }
                        }

                        LogosText {
                            visible: url.length > 0
                            Layout.fillWidth: true
                            text: url
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                            elide: Text.ElideRight
                        }

                        LogosText {
                            visible: description.length > 0
                            Layout.fillWidth: true
                            text: description
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        LogosText {
                            visible: resolveError.length > 0
                            Layout.fillWidth: true
                            text: resolveError
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.error
                            wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spacing.small
                            spacing: Theme.spacing.medium

                            RowLayout {
                                spacing: Theme.spacing.small
                                LogosSwitch {
                                    checked: isEnabled
                                    onToggled: root.setEnabledRequested(url, checked)
                                }
                                LogosText {
                                    text: isEnabled ? qsTr("Enabled") : qsTr("Disabled")
                                    color: Theme.palette.textSecondary
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                            }

                            Item { Layout.fillWidth: true }

                            LogosButton {
                                visible: !isDefault
                                text: qsTr("Remove")
                                implicitWidth: 100
                                implicitHeight: 32
                                onClicked: root.removeRequested(url)
                            }
                        }
                    }
                }
            }

            LogosText {
                visible: root.repositories.length === 0
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: root.loading
                      ? qsTr("Loading repositories…")
                      : qsTr("No repositories configured.")
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
            }

            // Divider above the add form.
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacing.medium
                Layout.preferredHeight: 1
                color: Theme.palette.borderSubtle
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.small

                LogosText {
                    text: qsTr("Add a repository")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }
                LogosText {
                    text: qsTr("Paste the URL of a logos-repo.json index.")
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textTertiary
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spacing.tiny
                    spacing: Theme.spacing.small

                    LogosTextField {
                        id: urlInput
                        Layout.fillWidth: true
                        placeholderText: qsTr("https://example.com/logos-repo.json")
                        text: d.newRepoUrl
                        onTextChanged: if (text !== d.newRepoUrl) d.newRepoUrl = text
                    }
                    LogosButton {
                        text: qsTr("Add")
                        enabled: d.newRepoUrl.trim().length > 0
                        implicitWidth: 100
                        implicitHeight: 40
                        onClicked: root.addRequested(d.newRepoUrl.trim())
                    }
                }
            }

            Item { Layout.fillHeight: true; Layout.minimumHeight: 1 }
        }
    }
}
