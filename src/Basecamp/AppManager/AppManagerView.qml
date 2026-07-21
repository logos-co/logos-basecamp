import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Theme

import Basecamp.Backend
import Basecamp.Common

Rectangle {
    id: root

    // ─── Public API ───
    property var appsProxy: null
    property var repositories: []
    property bool loading: false
    signal appClicked(string name, string repositoryUrl)
    signal manageAppRequested(string name, string repositoryUrl)
    signal navigateToRepositories()
    signal refreshRequested()

    QtObject {
        id: d

        readonly property string installStateFilter: {
            switch (stateTabBar.currentIndex) {
            case 1:  return "installed"
            case 2:  return "notInstalled"
            default: return "all"
            }
        }
        onInstallStateFilterChanged:
            if (root.appsProxy) root.appsProxy.installStateFilter = installStateFilter

        property string searchText: ""
        onSearchTextChanged:
            if (root.appsProxy) root.appsProxy.searchText = searchText

        property int    selectedCategoryIndex: 0   // 0 = "All"
        property string viewMode: "grid"

        readonly property var categories:
            root.appsProxy ? root.appsProxy.categories : ["All"]

        readonly property string selectedCategory:
            d.categories[d.selectedCategoryIndex] || "All"
        onSelectedCategoryChanged:
            if (root.appsProxy) root.appsProxy.categoryFilter = selectedCategory
    }

    // Initial sync — push d's current values onto the proxy as soon as
    // it's assigned. The change handlers inside `d` keep things in sync
    // after that.
    onAppsProxyChanged: {
        if (appsProxy) {
            appsProxy.categoryFilter     = d.selectedCategory
            appsProxy.searchText         = d.searchText
            appsProxy.installStateFilter = d.installStateFilter
        }
    }

    color: Theme.palette.background

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.xxlarge
        spacing: Theme.spacing.xlarge

        // Header: title + subtitle on the left, search on the right.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.xlarge

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.tiny

                LogosText {
                    text: "Applications"
                    font.pixelSize: Theme.typography.pageTitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }

                LogosText {
                    text: "Install and manage applications."
                    font.pixelSize: Theme.typography.primaryText
                    color: Theme.palette.textSecondary
                }
            }

            Item { Layout.fillWidth: true }

            LogosSearchBar {
                id: searchBar
                Layout.alignment: Qt.AlignRight
                Layout.preferredWidth: 605
                Layout.minimumWidth: 200
                text: d.searchText
                placeholderText: qsTr("Search apps…")
                shortcutHint: "⌘K"
                onTextChanged: {
                    if (text !== d.searchText)
                        d.searchText = text
                }
            }

            // ⌘K focuses + selects the search bar.
            Shortcut {
                sequence: "Ctrl+K"
                context: Qt.WindowShortcut
                onActivated: {
                    searchBar.textInput.forceActiveFocus()
                    searchBar.textInput.selectAll()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing.medium

            // ─── Categories sidebar ───
            Item {
                Layout.preferredWidth: 200
                Layout.minimumWidth: 160
                Layout.maximumWidth: 200
                Layout.fillHeight: true

                Flickable {
                    id: categoriesScroll
                    anchors.fill: parent
                    clip: true
                    contentWidth: width
                    contentHeight: categoriesCol.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: LogosScrollBar {
                        policy: ScrollBar.AsNeeded
                        visible: categoriesScroll.contentHeight > categoriesScroll.height
                    }

                    ColumnLayout {
                        id: categoriesCol
                        width: categoriesScroll.width
                        spacing: Theme.spacing.tiny

                        LogosText {
                            Layout.topMargin: Theme.spacing.tiny
                            Layout.bottomMargin: Theme.spacing.tiny
                            text: qsTr("Categories")
                            font.pixelSize: Theme.typography.subtitleText
                            font.weight: Theme.typography.weightRegular
                            color: Theme.palette.text
                        }

                        ListView {
                            Layout.fillWidth: true
                            Layout.preferredHeight: contentHeight
                            interactive: false
                            spacing: Theme.spacing.tiny
                            model: d.categories
                            currentIndex: d.selectedCategoryIndex

                            delegate: SidebarNavItem {
                                width: ListView.view.width
                                text: modelData
                                highlighted: ListView.isCurrentItem
                                onClicked: d.selectedCategoryIndex = index
                            }
                        }
                    }
                }

                component SidebarNavItem: LogosItemDelegate {
                    id: cell
                    radius: Theme.spacing.radiusLarge
                    highlightColor: Theme.palette.backgroundButton
                    hoverColor: "transparent"
                    textColor: (cell.highlighted || cell.hovered)
                                   ? Theme.palette.text
                                   : Theme.palette.textTertiary
                }
            }

            // ─── Apps panel ───
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.palette.surfaceRaised
                radius: Theme.spacing.radiusXlarge
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.large
                    spacing: Theme.spacing.medium

                    // ─── Panel header: title + install-state tabs + view toggle ───
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing.xlarge

                        LogosText {
                            Layout.preferredHeight: implicitHeight
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

                        LogosButton {
                            id: reloadBtn
                            objectName: "appManager.reloadButton"
                            Layout.minimumWidth: 80
                            Layout.preferredWidth: 130
                            Layout.maximumWidth: 130
                            Layout.preferredHeight: 40
                            radius: Theme.spacing.radiusLarge
                            text: qsTr("Reload")
                            icon.source: LogosIcons.refresh
                            icon.size: 18
                            enabled: !root.loading
                            onClicked: root.refreshRequested()
                        }

                        LogosButton {
                            Layout.minimumWidth: 100
                            Layout.preferredWidth: 150
                            Layout.maximumWidth: 150
                            Layout.preferredHeight: 40
                            radius: Theme.spacing.radiusLarge
                            text: qsTr("Manage Repositories")
                            onClicked: root.navigateToRepositories()
                        }

                        RowLayout {
                            spacing: 24
                            Layout.preferredHeight: 36

                            LogosText {
                                Layout.alignment: Qt.AlignVCenter
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("View:")
                                color:  Theme.palette.textTertiary
                            }

                            LogosIconButton {
                                iconSource: LogosIcons.grid
                                size: 20
                                iconSize: 20
                                iconColor: d.viewMode === "grid"
                                           ? Theme.palette.text
                                           : Theme.palette.textTertiary
                                onClicked: d.viewMode = "grid"
                                background: Item{}
                            }

                            LogosIconButton {
                                iconSource: LogosIcons.list
                                size: 20
                                iconSize: 20
                                iconColor: d.viewMode === "list"
                                           ? Theme.palette.text
                                           : Theme.palette.textTertiary
                                onClicked: d.viewMode = "list"
                                background: Item{}
                            }
                        }
                    }

                    // Empty state — shown when no repositories are configured.
                    // Replaces the grid so the user gets a direct call-to-action.
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: root.repositories.length === 0 && !root.loading

                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: Theme.spacing.medium
                            width: Math.min(parent.width * 0.6, 420)

                            LogosText {
                                Layout.alignment: Qt.AlignHCenter
                                text: qsTr("No repositories configured")
                                font.pixelSize: Theme.typography.subtitleText
                                font.weight: Theme.typography.weightMedium
                                color: Theme.palette.text
                                horizontalAlignment: Text.AlignHCenter
                            }

                            LogosText {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.fillWidth: true
                                text: qsTr("Add a package repository to browse and install apps.")
                                font.pixelSize: Theme.typography.primaryText
                                color: Theme.palette.textSecondary
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                            }

                            LogosButton {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.topMargin: Theme.spacing.small
                                text: qsTr("Manage Repositories")
                                onClicked: root.navigateToRepositories()
                            }
                        }
                    }

                    Flickable {
                        id: gridScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: root.repositories.length > 0 || root.loading
                        clip: true
                        contentWidth: width
                        contentHeight: gridColumn.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds

                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                        }

                        ColumnLayout {
                            id: gridColumn
                            width: gridScroll.width
                            spacing: Theme.spacing.large

                            Repeater {
                                model: root.repositories
                                delegate: ColumnLayout {
                                    required property var modelData

                                    AppsFilterProxy {
                                        id: repoFilter
                                        sourceModel: root.appsProxy
                                        repositoryUrlFilter: modelData.url || ""
                                        excludeMainUi: false
                                    }

                                    Layout.fillWidth: true
                                    spacing: Theme.spacing.medium
                                    visible: modelData.enabled !== false
                                             && repoFilter.visibleCount > 0

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacing.small

                                        LogosText {
                                            text: modelData.isDefault === true
                                                ? qsTr("Starter Apps")
                                                : (modelData.displayName
                                                   || modelData.name
                                                   || modelData.url
                                                   || qsTr("Repository"))
                                            font.pixelSize: Theme.typography.subtitleText
                                            font.weight: Theme.typography.weightMedium
                                            color: Theme.palette.textSecondary
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        LogosText {
                                            text: "(" + repoFilter.visibleCount + ")"
                                            font.pixelSize: Theme.typography.secondaryText
                                            color: Theme.palette.textTertiary
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: Theme.palette.borderSubtle
                                        opacity: 0.5
                                    }

                                    AppGrid {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: implicitHeight
                                        modulesSource: repoFilter
                                        viewMode: d.viewMode
                                        onAppClicked: function(name, repositoryUrl) {
                                            root.appClicked(name, repositoryUrl || "")
                                        }
                                        onAppManageRequested: function(name, repositoryUrl) {
                                            root.manageAppRequested(name, repositoryUrl || "")
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Global loading overlay — matches Package Manager UI's Reload feedback.
    LoadingOverlay {
        anchors.fill: parent
        visible: root.loading
    }
}
