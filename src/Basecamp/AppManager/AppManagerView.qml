import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Layouts
import Logos.Theme

import Basecamp.Backend
import Basecamp.Common

Rectangle {
    id: root
    // For UI tests: `loading` says when the catalog is in.
    objectName: "appManager.view"

    // ─── Public API ───
    property var appsProxy: null

    // Applies a query from outside the view — the welcome page's search hands
    // its text over when a result routes here. Into the field, so the field
    // and the proxy stay in step.
    function applySearch(query) { header.openSearch(query) }
    property var repositories: []
    property bool loading: false
    property bool showUnavailable: false

    // This host's variant, for the "nothing for this platform" empty state.
    property string hostVariant: ""
    // Why downloads may not work right now, for the notice under the header.
    property bool online: true
    property string downloadSource: ""
    property string storageState: ""
    signal appClicked(string name, string repositoryUrl)
    signal manageAppRequested(string name, string repositoryUrl)
    signal uninstallAppRequested(string name, string repositoryUrl)
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

        readonly property string searchText: header.searchText
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
        showUnavailable: true
    }

    color: Theme.palette.background

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin(root.width)
        spacing: Theme.spacing.xlarge

        // ─── Title and subtitle, the search on their right; on a phone a 🔍
        // at the end of the title's line ───
        LogosPageHeader {
            id: header
            objectName: "appManager.header"
            Layout.fillWidth: true
            title: qsTr("Applications")
            subtitle: qsTr("Install and manage applications.")
            // On a phone it would only say the title again.
            subtitleOnPhone: false
            searchEnabled: true
            searchPlaceholder: qsTr("Search apps…")
            searchShortcutHint: searchShortcut.nativeText
            searchWidth: 605
            searchItem.objectName: "appManager.searchField"
        }

        // ⌘K focuses + selects the search.
        Shortcut {
            id: searchShortcut
            objectName: "appManager.searchShortcut"
            sequence: "Ctrl+K"
            context: Qt.WindowShortcut
            enabled: root.visible
            onActivated: header.openSearch()
        }

        // The App Manager does not show where each download comes from, so
        // Logos Storage being down while GitHub covers for it is not news here.
        LogosNotice {
            id: downloadNotice
            objectName: "appManager.downloadNotice"
            readonly property var notice: DownloadFormat.downloadNotice(
                root.online, root.downloadSource, root.storageState, false)
            Layout.fillWidth: true
            shown: notice !== null
            severity: !notice ? LogosNotice.Info
                    : notice.severity === "error" ? LogosNotice.Error
                    : notice.severity === "warning" ? LogosNotice.Warning
                    : LogosNotice.Info
            title: notice ? notice.title : ""
            message: notice ? notice.message : ""
        }

        LogosSidebarLayout {
            id: body
            objectName: "appManager.body"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing.medium

            side: LogosListView {
                id: categoriesList

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
                    objectName: "appManager.category." + modelData
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
                anchors.fill: parent
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
                        objectName: "appManager.panelHeader"
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.spacing.medium
                        Layout.rightMargin: Theme.spacing.medium

                        loading: root.loading
                        viewMode: d.viewMode
                        showUnavailable: root.showUnavailable
                        showCategoryPicker: !body.sideShown
                        categories: d.categories
                        categoryIndex: d.selectedCategoryIndex
                        onCategorySelected: (index) => d.selectedCategoryIndex = index
                        onReloadClicked: root.refreshRequested()
                        onRepositoriesClicked: root.navigateToRepositories()
                        onViewModeChangeRequested: (mode) => d.viewMode = mode
                        onShowUnavailableRequested: (show) => root.showUnavailable = show
                    }

                    // Empty state — shown when no repositories are configured
                    // AND there are no local-only installed rows to fall back on.
                    // If either exists, we render the grid instead.
                    EmptyView {
                        objectName: "appManager.noReposView"
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
                                        showUnavailable: true
                                    }

                                    title: modelData.displayLabel
                                           || modelData.displayName
                                           || modelData.name
                                           || modelData.url
                                           || qsTr("Repository")
                                    count: repoFilter.visibleCount
                                    modulesSource: repoFilter
                                    viewMode: d.viewMode
                                    visible: modelData.enabled !== false
                                             && repoFilter.visibleCount > 0

                                    onAppClicked: (name, url) => root.appClicked(name, url)
                                    onAppManageRequested: (name, url) => root.manageAppRequested(name, url)
                                    onAppUninstallRequested: (name, url) => root.uninstallAppRequested(name, url)
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
                                    showUnavailable: true
                                }

                                title: qsTr("local")
                                count: localFilter.visibleCount
                                modulesSource: localFilter
                                viewMode: d.viewMode
                                visible: localFilter.visibleCount > 0

                                onAppClicked: (name, url) => root.appClicked(name, url)
                                onAppManageRequested: (name, url) => root.manageAppRequested(name, url)
                                onAppUninstallRequested: (name, url) => root.uninstallAppRequested(name, url)
                            }

                            // Empty-search state — every section above hides
                            // itself when the search matches nothing, so fill
                            // the viewport with feedback instead of a blank
                            // panel. Search-only: an empty search keeps the
                            // no-repos EmptyView / category filtering as-is.
                            EmptyView {
                                objectName: "appManager.emptyView"
                                Layout.fillWidth: true
                                Layout.preferredHeight: gridScroll.height
                                visible: d.searchText.length > 0
                                         && root.appsProxy !== null
                                         && root.appsProxy.visibleCount === 0

                                title: qsTr("No apps match your search.")
                                subtitle: qsTr("Try a different search term.")
                            }

                            EmptyView {
                                objectName: "appManager.noneForPlatformView"
                                Layout.fillWidth: true
                                Layout.preferredHeight: gridScroll.height
                                visible: d.searchText.length === 0
                                         && d.selectedCategoryIndex === 0
                                         && !root.showUnavailable
                                         && !root.loading
                                         && root.repositories.length > 0
                                         && root.appsProxy !== null
                                         && root.appsProxy.visibleCount === 0

                                title: qsTr("Nothing available for this platform")
                                subtitle: (root.hostVariant.length > 0
                                           && !root.hostVariant.startsWith("unknown"))
                                          ? qsTr("No configured repository publishes a build for %1.")
                                                .arg(root.hostVariant)
                                          : qsTr("No configured repository publishes a build for this platform.")
                            }
                        }
                    }
                }
            }
        }
    }

    // Global loading overlay — matches Package Manager UI's Reload feedback.
    LoadingOverlay {
        objectName: "appManager.loadingOverlay"
        anchors.fill: parent
        visible: root.loading
    }
}
