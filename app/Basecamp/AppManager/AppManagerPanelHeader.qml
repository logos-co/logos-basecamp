import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Theme

Item {
    id: root

    property alias installStateIndex: stateTabBar.currentIndex
    property string viewMode: "grid"
    property bool loading: false

    signal reloadClicked()
    signal repositoriesClicked()
    signal viewModeChangeRequested(string mode)

    implicitHeight: panelHeader.implicitHeight

    GridLayout {
        id: panelHeader
        anchors.left: parent.left
        anchors.right: parent.right
        columnSpacing: Theme.spacing.large
        rowSpacing: Theme.spacing.medium
        columns: (leftHalf.implicitWidth + rightHalf.implicitWidth + columnSpacing) <= width
                 ? 2 : 1

        RowLayout {
            id: leftHalf
            Layout.fillWidth: true
            spacing: Theme.spacing.large

            LogosText {
                text: qsTr("Apps")
                font.pixelSize: Theme.typography.panelTitleText
                font.weight: Theme.typography.weightMedium
                color: Theme.palette.text
            }

            LogosTabBar {
                id: stateTabBar
                spacing: Theme.spacing.large

                LogosTabButton { text: qsTr("All");           iconSource: LogosIcons.pages }
                LogosTabButton { text: qsTr("Installed") }
                LogosTabButton { text: qsTr("Not Installed") }
            }

            Item { Layout.fillWidth: true }
        }

        RowLayout {
            id: rightHalf
            Layout.fillWidth: true
            spacing: Theme.spacing.medium

            Item { Layout.fillWidth: panelHeader.columns === 2 }

            LogosButton {
                id: reloadBtn
                objectName: "appManager.reloadButton"
                Layout.fillWidth: true
                Layout.minimumWidth: 80
                Layout.preferredWidth: 100
                Layout.maximumWidth: 100
                Layout.preferredHeight: 40
                radius: Theme.spacing.radiusLarge
                text: qsTr("Reload")
                leadingIcon.source: LogosIcons.refresh
                leadingIcon.size: 18
                enabled: !root.loading
                onClicked: root.reloadClicked()
            }

            LogosButton {
                Layout.fillWidth: true
                Layout.minimumWidth: 100
                Layout.preferredWidth: 130
                Layout.maximumWidth: 130
                Layout.preferredHeight: 40
                radius: Theme.spacing.radiusLarge
                text: qsTr("Repositories")
                onClicked: root.repositoriesClicked()
            }

            RowLayout {
                spacing: 24
                Layout.preferredHeight: 36

                LogosText {
                    Layout.alignment: Qt.AlignVCenter
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("View:")
                    color: Theme.palette.textTertiary
                }

                LogosIconButton {
                    iconSource: LogosIcons.grid
                    size: 20
                    iconSize: 20
                    iconColor: root.viewMode === "grid"
                               ? Theme.palette.text
                               : Theme.palette.textTertiary
                    onClicked: root.viewModeChangeRequested("grid")
                    background: Item {}
                }

                LogosIconButton {
                    iconSource: LogosIcons.list
                    size: 20
                    iconSize: 20
                    iconColor: root.viewMode === "list"
                               ? Theme.palette.text
                               : Theme.palette.textTertiary
                    onClicked: root.viewModeChangeRequested("list")
                    background: Item {}
                }
            }
        }
    }
}
