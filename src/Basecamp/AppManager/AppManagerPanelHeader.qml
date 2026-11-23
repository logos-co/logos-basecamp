import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Layouts
import Logos.Theme

import Basecamp.Icons

LogosItemLayout {
    id: root

    property alias installStateIndex: stateTabBar.currentIndex
    property string viewMode: "grid"
    property bool loading: false
    property bool showUnavailable: false
    property bool showCategoryPicker: false
    property var categories: []
    property int categoryIndex: 0

    signal reloadClicked()
    signal repositoriesClicked()
    signal viewModeChangeRequested(string mode)
    signal showUnavailableRequested(bool show)
    signal categorySelected(int index)

    spacing: Theme.spacing.large
    minimumContentWidth: stateTabBar.implicitWidth

    Item {
        implicitWidth: stateTabBar.implicitWidth
        implicitHeight: stateTabBar.implicitHeight

        LogosTabBar {
            id: stateTabBar
            objectName: "appManager.stateTabs"
            width: implicitWidth
            spacing: Theme.spacing.large

            LogosTabButton { text: qsTr("All");           iconSource: LogosIcons.pages }
            LogosTabButton { text: qsTr("Installed") }
            LogosTabButton { text: qsTr("Not Installed") }
        }
    }

    // ─── Toolbar: the picker, then the actions, folding into ⋯ ───
    meta: [
        LogosComboBox {
            objectName: "appManager.categoryCombo"
            visible: root.showCategoryPicker
            Layout.fillWidth: true
            Layout.minimumWidth: Theme.size.touchTarget * 2
            Layout.maximumWidth: 220
            model: root.categories
            currentIndex: root.categoryIndex
            displayText: currentIndex <= 0 ? qsTr("All categories") : currentText
            onActivated: function(index) { root.categorySelected(index) }
        },

        LogosOverflowRow {
            objectName: "appManager.toolbarActions"
            Layout.fillWidth: true
            Layout.minimumWidth: Theme.size.touchTarget
            spacing: Theme.spacing.medium

            LogosIconButton {
                id: filterButton
                objectName: "appManager.filterButton"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                size: 40
                iconSize: 16
                iconSource: BasecampIcons.filter
                iconColor: root.showUnavailable
                           ? Theme.palette.primary
                           : Theme.palette.text
                onClicked: filterMenu.popupUnder(filterButton)

                background: Rectangle {
                    radius: Theme.spacing.radiusLarge
                    color: (root.showUnavailable || filterButton.hovered
                            || filterButton.pressed || filterButton.activeFocus)
                           ? Theme.palette.backgroundMuted
                           : "transparent"
                }

                LogosToolTip {
                    text: root.showUnavailable
                          ? qsTr("Filters: showing unavailable")
                          : qsTr("Filter apps")
                    placement: LogosToolTip.Bottom
                    visible: filterButton.hovered && !filterMenu.visible
                }
            }

            LogosMenu {
                id: filterMenu
                objectName: "appManager.filterMenu"

                CheckItem {
                    objectName: "appManager.showUnavailableItem"
                    text: qsTr("Show unavailable")
                    checked: root.showUnavailable
                    onTriggered: root.showUnavailableRequested(checked)
                }
            }

            LogosButton {
                objectName: "appManager.reloadButton"
                Layout.preferredWidth: 100
                Layout.preferredHeight: 40
                radius: Theme.spacing.radiusLarge
                text: qsTr("Reload")
                leadingIcon.source: LogosIcons.refresh
                leadingIcon.size: 18
                enabled: !root.loading
                onClicked: root.reloadClicked()
            }

            LogosButton {
                objectName: "appManager.repositoriesButton"
                Layout.preferredHeight: 40
                radius: Theme.spacing.radiusLarge
                text: qsTr("Manage Repositories")
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
                    objectName: "appManager.gridViewButton"
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
                    objectName: "appManager.listViewButton"
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

            // Grid or list: picking the one shown keeps it.
            ButtonGroup { id: viewModes }

            // Folded, the same in a menu.
            menu: LogosMenu {
                objectName: "appManager.moreMenu"

                CheckItem {
                    objectName: "appManager.moreShowUnavailableItem"
                    text: qsTr("Show unavailable")
                    checked: root.showUnavailable
                    onTriggered: root.showUnavailableRequested(checked)
                }

                LogosMenuSeparator {}

                CheckItem {
                    objectName: "appManager.moreGridItem"
                    ButtonGroup.group: viewModes
                    text: qsTr("Grid")
                    checked: root.viewMode === "grid"
                    onTriggered: root.viewModeChangeRequested("grid")
                }

                CheckItem {
                    objectName: "appManager.moreListItem"
                    ButtonGroup.group: viewModes
                    text: qsTr("List")
                    checked: root.viewMode === "list"
                    onTriggered: root.viewModeChangeRequested("list")
                }

                LogosMenuSeparator {}

                LogosMenuItem {
                    objectName: "appManager.moreReloadItem"
                    text: qsTr("Reload")
                    enabled: !root.loading
                    onTriggered: root.reloadClicked()
                }

                LogosMenuItem {
                    objectName: "appManager.moreRepositoriesItem"
                    text: qsTr("Manage Repositories…")
                    onTriggered: root.repositoriesClicked()
                }
            }
        }
    ]

    // A menu item with the design system's check mark when checked.
    component CheckItem: LogosMenuItem {
        id: checkItem
        readonly property int indicatorSize: 14
        checkable: true

        indicator: LogosIcon {
            anchors.verticalCenter: parent.verticalCenter
            x: Theme.spacing.medium
            width: checkItem.indicatorSize
            height: checkItem.indicatorSize
            visible: checkItem.checked
            source: LogosIcons.check
            color: Theme.palette.text
        }

        contentItem: LogosText {
            leftPadding: Theme.spacing.medium
                         + checkItem.indicatorSize
                         + Theme.spacing.small
            rightPadding: Theme.spacing.medium
            verticalAlignment: Text.AlignVCenter
            text: checkItem.text
            color: checkItem.enabled
                   ? Theme.palette.text
                   : Theme.palette.textMuted
        }
    }
}
