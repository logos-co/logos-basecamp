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
            switch (panelHeader.installStateIndex) {
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

    // Filter proxy for the "Local" section — installed packages with no
    // catalog row (empty repositoryUrl). Hoisted out of the grid so the
    // empty-state guard below can gate on its visibleCount. objectName is
    // the test anchor (tests/ui-tests.mjs looks it up via findByProperty).
    AppsFilterProxy {
        id: localAppsProxy
        objectName: "appManager.localAppsProxy"
        sourceModel: root.appsProxy
        matchLocalOnly: true
        excludeMainUi: false
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
                enabled: root.visible
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
            LogosListView {
                id: categoriesList

                Layout.preferredWidth: 200
                Layout.minimumWidth: 160
                Layout.maximumWidth: 200
                Layout.fillHeight: true

                model: d.categories
                currentIndex: d.selectedCategoryIndex

                header: LogosText {
                    width: categoriesList.width
                    topPadding: Theme.spacing.tiny
                    bottomPadding: Theme.spacing.tiny
                    text: qsTr("Categories")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightRegular
                    color: Theme.palette.text
                }

                delegate: LogosItemDelegate {
                    id: cell
                    width: ListView.view.width
                    text: modelData
                    highlighted: ListView.isCurrentItem
                    radius: Theme.spacing.radiusLarge
                    highlightColor: Theme.palette.backgroundButton
                    hoverColor: "transparent"
                    textColor: (cell.highlighted || cell.hovered)
                                   ? Theme.palette.text
                                   : Theme.palette.textTertiary
                    onClicked: d.selectedCategoryIndex = index
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
                    anchors.topMargin: Theme.spacing.medium
                    anchors.bottomMargin: Theme.spacing.medium
                    spacing: Theme.spacing.medium

                    AppManagerPanelHeader {
                        id: panelHeader
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.spacing.medium
                        Layout.rightMargin: Theme.spacing.medium

                        loading: root.loading
                        viewMode: d.viewMode
                        onReloadClicked: root.refreshRequested()
                        onRepositoriesClicked: root.navigateToRepositories()
                        onViewModeChangeRequested: (mode) => d.viewMode = mode
                    }

                    // Empty state — shown when no repositories are configured
                    // AND there are no local-only installed rows to fall back on.
                    // If either exists, we render the grid instead.
                    EmptyView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: root.repositories.length === 0
                                 && localAppsProxy.visibleCount === 0
                                 && !root.loading

                        title: qsTr("No repositories configured")
                        subtitle: qsTr("Add a package repository to browse and install apps.")
                        actionText: qsTr("Manage Repositories")
                        onActionClicked: root.navigateToRepositories()
                    }

                    Flickable {
                        id: gridScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: root.repositories.length > 0
                                 || localAppsProxy.visibleCount > 0
                                 || root.loading
                        clip: true
                        contentWidth: width
                        contentHeight: gridColumn.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds

                        ScrollBar.vertical: LogosScrollBar {
                            policy: ScrollBar.AsNeeded
                            rightPadding: 4
                        }

                        ColumnLayout {
                            id: gridColumn
                            x: Theme.spacing.medium
                            width: gridScroll.width - 2 * Theme.spacing.medium
                            spacing: Theme.spacing.large

                            Repeater {
                                model: root.repositories
                                delegate: AppRepoSection {
                                    required property var modelData

                                    AppsFilterProxy {
                                        id: repoFilter
                                        sourceModel: root.appsProxy
                                        repositoryUrlFilter: modelData.url || ""
                                        excludeMainUi: false
                                    }

                                    title: modelData.isDefault === true
                                        ? qsTr("Starter Apps")
                                        : (modelData.displayName
                                           || modelData.name
                                           || modelData.url
                                           || qsTr("Repository"))
                                    count: repoFilter.visibleCount
                                    modulesSource: repoFilter
                                    viewMode: d.viewMode
                                    visible: modelData.enabled !== false
                                             && repoFilter.visibleCount > 0

                                    onAppClicked: (name, url) => root.appClicked(name, url)
                                    onAppManageRequested: (name, url) => root.manageAppRequested(name, url)
                                }
                            }

                            // Local — synthetic "repo" section for installed
                            // packages the catalog doesn't claim. Sits after
                            // the real repos so it always renders last.
                            AppRepoSection {
                                AppsFilterProxy {
                                    id: localFilter
                                    sourceModel: root.appsProxy
                                    matchLocalOnly: true
                                    excludeMainUi: false
                                }

                                title: qsTr("local")
                                count: localFilter.visibleCount
                                modulesSource: localFilter
                                viewMode: d.viewMode
                                visible: localFilter.visibleCount > 0

                                onAppClicked: (name, url) => root.appClicked(name, url)
                                onAppManageRequested: (name, url) => root.manageAppRequested(name, url)
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
