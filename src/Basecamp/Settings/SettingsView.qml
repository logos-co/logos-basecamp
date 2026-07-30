import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

Rectangle {
    id: root

    property var    repositories:        []
    property bool   repositoriesLoading: false
    // Real Qt models (ModuleInstanceModel), owned by MainUIBackend and
    // populated on every uiModulesChanged/coreModulesChanged tick.
    property var    uiModulesModel:      null
    property var    coreModulesModel:    null
    property bool   modulesLoading:      false

    signal repositoryRefreshRequested()
    signal repositoryAddRequested(string url)
    signal repositoryRemoveRequested(string url)
    signal repositoryEnabledRequested(string url, bool enabled)
    signal repositoriesBecameVisible()

    signal appsRefreshRequested()
    signal appLoadRequested(string name)
    signal appUnloadRequested(string name)
    signal appsInspectorBecameVisible()

    signal modulesRefreshRequested()
    signal moduleLoadRequested(string name)
    signal moduleUnloadRequested(string name)
    signal moduleInspectorBecameVisible()

    function reportRepositoryResult(operation, url, success, error) {
        repositoriesView.reportOperationResult(operation, url, success, error)
    }

    function showRepositories() { d.selectedIndex = d.sectionRepositories }

    QtObject {
        id: d

        // Sub-views in the right pane. Order maps 1:1 to the StackLayout below.
        readonly property int sectionDashboard:       0
        readonly property int sectionAppsInspector:   1
        readonly property int sectionModuleInspector: 2
        readonly property int sectionRepositories:    3

        readonly property var sections: [
            { label: qsTr("Dashboard") },
            { label: qsTr("Apps Inspector") },
            { label: qsTr("Module Inspector") },
            { label: qsTr("Package Repositories") }
        ]

        property int selectedIndex: 0

        // Search text is shared by the two inspectors. Reset whenever the
        // user switches away so stale queries don't leak between panels.
        property string searchText: ""

        readonly property bool searchable:
            selectedIndex === sectionAppsInspector ||
            selectedIndex === sectionModuleInspector

        onSelectedIndexChanged: searchText = ""
    }

    color: Theme.palette.background

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.xxlarge
        spacing: Theme.spacing.xlarge

        // ─── Header ───
        // Title + subtitle on the left; when an inspector is selected, a
        // page-level search bar joins on the right — same layout as
        // AppManagerView so the two feel familiar side by side.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.xlarge

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.tiny

                LogosText {
                    text: qsTr("Settings")
                    font.pixelSize: Theme.typography.pageTitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }

                LogosText {
                    text: qsTr("Manage modules, apps and dashboards.")
                    font.pixelSize: Theme.typography.primaryText
                    color: Theme.palette.textSecondary
                }
            }

            Item { Layout.fillWidth: true }

            LogosSearchBar {
                id: searchBar
                visible: d.searchable
                Layout.alignment: Qt.AlignRight
                Layout.preferredWidth: 605
                Layout.minimumWidth: 200
                text: d.searchText
                placeholderText: d.selectedIndex === d.sectionAppsInspector
                                 ? qsTr("Search apps…")
                                 : qsTr("Search modules…")
                shortcutHint: "⌘K"
                onTextChanged: {
                    if (text !== d.searchText)
                        d.searchText = text
                }
            }

            Shortcut {
                sequence: "Ctrl+K"
                context: Qt.WindowShortcut
                enabled: root.visible && d.searchable
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

            // ─── Sections sidebar ───
            LogosListView {
                id: sectionsList

                Layout.preferredWidth: 200
                Layout.minimumWidth: 160
                Layout.maximumWidth: 200
                Layout.fillHeight: true

                model: d.sections
                currentIndex: d.selectedIndex

                header: LogosText {
                    width: sectionsList.width
                    topPadding: Theme.spacing.tiny
                    bottomPadding: Theme.spacing.tiny
                    text: qsTr("Sections")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightRegular
                    color: Theme.palette.text
                }

                delegate: LogosItemDelegate {
                    id: cell
                    width: ListView.view.width
                    text: modelData.label
                    highlighted: ListView.isCurrentItem
                    radius: Theme.spacing.radiusLarge
                    highlightColor: Theme.palette.backgroundButton
                    hoverColor: "transparent"
                    textColor: (cell.highlighted || cell.hovered)
                                   ? Theme.palette.text
                                   : Theme.palette.textTertiary
                    onClicked: d.selectedIndex = index
                }
            }

            // ─── Right pane ───
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.palette.surfaceRaised
                radius: Theme.spacing.radiusXlarge
                clip: true

                StackLayout {
                    anchors.fill: parent
                    currentIndex: d.selectedIndex

                    // 0 — Dashboard.
                    DashboardView {}

                    // 1 — Apps Inspector (UI plugins).
                    AppsInspectorView {
                        sourceModel: root.uiModulesModel
                        loading:     root.modulesLoading
                        searchText:  d.searchText

                        onReloadRequested: root.appsRefreshRequested()
                        onLoadRequested:   name => root.appLoadRequested(name)
                        onUnloadRequested: name => root.appUnloadRequested(name)
                        onVisibleChanged:  if (visible) root.appsInspectorBecameVisible()
                    }

                    // 2 — Module Inspector (core modules).
                    ModuleInspectorView {
                        sourceModel: root.coreModulesModel
                        loading:     root.modulesLoading
                        searchText:  d.searchText

                        onReloadRequested: root.modulesRefreshRequested()
                        onLoadRequested:   name => root.moduleLoadRequested(name)
                        onUnloadRequested: name => root.moduleUnloadRequested(name)
                        onVisibleChanged:  if (visible) root.moduleInspectorBecameVisible()
                    }

                    // 3 — Package Repositories.
                    RepositoriesView {
                        id: repositoriesView

                        repositories: root.repositories
                        loading:      root.repositoriesLoading

                        onRefreshRequested:    root.repositoryRefreshRequested()
                        onAddRequested:        url => root.repositoryAddRequested(url)
                        onRemoveRequested:     url => root.repositoryRemoveRequested(url)
                        onSetEnabledRequested: (url, enabled) =>
                                                   root.repositoryEnabledRequested(url, enabled)
                        onVisibleChanged:      if (visible)
                                                   root.repositoriesBecameVisible()
                    }
                }
            }
        }
    }
}
