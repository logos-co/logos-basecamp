import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Layouts
import Logos.Theme

Rectangle {
    id: root

    property var    repositories:        []
    property bool   repositoriesLoading: false
    property string downloadSource:      ""
    // Real Qt models (ModuleInstanceModel), owned by MainUIBackend and
    // populated on every uiModulesChanged/coreModulesChanged tick.
    property var    uiModulesModel:      null
    property var    coreModulesModel:    null
    property bool   modulesLoading:      false
    // False where the host manages no package repositories.
    property bool   repositoriesAvailable: true
    property var    trustedPublishers:   []
    property var    installedFromPublisher: function(did) { return [] }
    // The build, for the Dashboard.
    property string buildVersion:        ""
    property bool   portableBuild:       false
    property var    buildCommits:        []

    signal repositoryRefreshRequested()
    signal repositoryAddRequested(string url)
    signal repositoryRemoveRequested(string url)
    signal repositoryEnabledRequested(string url, bool enabled)
    signal downloadSourceRequested(string source)
    signal repositoriesBecameVisible()

    signal trustedPublisherRemoveRequested(string keyName)
    signal trustedPublishersRefreshRequested()

    signal appsRefreshRequested()
    signal appLoadRequested(string name)
    signal appUnloadRequested(string name)
    signal appsInspectorBecameVisible()

    signal modulesRefreshRequested()
    signal moduleLoadRequested(string name)
    signal moduleUnloadRequested(string name)
    signal moduleInspectorBecameVisible()

    signal themeInstallRequested(url file)
    signal themeSelectRequested(string themeId)
    signal themeRemoveRequested(string themeId)

    function reportRepositoryResult(operation, url, success, error) {
        repositoriesView.reportOperationResult(operation, url, success, error)
    }

    function showThemeInstallResult(result) { appearanceView.showInstallResult(result) }
    function showThemeRemoveResult(result)  { appearanceView.showRemoveResult(result) }
    function reportPublisherFailure(message) {
        trustedPublishersView.reportFailure(message)
    }

    function showRepositories() { if (root.repositoriesAvailable) settingsSections.currentIndex = d.sectionRepositories }

    QtObject {
        id: d

        // Sub-views in the right pane. Order maps 1:1 to the StackLayout below.
        readonly property int sectionDashboard:       0
        readonly property int sectionAppsInspector:   1
        readonly property int sectionModuleInspector: 2
        readonly property int sectionAppearance:      3
        readonly property int sectionRepositories:    4

        readonly property var sections: [
            { title: qsTr("Dashboard") },
            { title: qsTr("Apps Inspector") },
            { title: qsTr("Module Inspector") },
            { title: qsTr("Appearance") },
            { title: qsTr("Package Repositories") },
            { title: qsTr("Trusted Publishers") }
        ].slice(0, root.repositoriesAvailable ? 6 : 4)   // Repositories and Trusted Publishers must stay last

        readonly property int selectedIndex: settingsSections.currentIndex

        // The open inspector, whose levels (a row's detail on a phone, a
        // module's Interface) Settings' one back row goes up through.
        readonly property Item openInspector: selectedIndex === sectionAppsInspector ? appsInspector
                                            : selectedIndex === sectionModuleInspector ? moduleInspector
                                            : null

        // The inspectors' table, not over a level of theirs.
        readonly property bool searchable: !!openInspector && !openInspector.canGoBack
    }

    color: Theme.palette.background

    LogosSections {
        id: settingsSections

        anchors.fill: parent
        title: qsTr("Settings")
        subtitle: qsTr("Manage modules, apps and dashboards.")
        listTitle: qsTr("Sections")
        sections: d.sections
        canGoBack: !!d.openInspector && d.openInspector.canGoBack
        backTitle: d.openInspector ? d.openInspector.backTitle : ""
        onBackRequested: d.openInspector.goBack()
        backRowItem.objectName: "settings.back"
        sectionBackRowItem.objectName: "settings.sectionBack"
        searchEnabled: d.searchable
        searchPlaceholder: d.selectedIndex === d.sectionAppsInspector
                           ? qsTr("Search apps…")
                           : qsTr("Search modules…")
        searchShortcutHint: searchShortcut.nativeText
        searchWidth: 605
        searchItem.objectName: "settings.searchField"
        onCurrentIndexChanged: closeSearch()

        Shortcut {
            id: searchShortcut
            objectName: "settings.searchShortcut"
            sequence: "Ctrl+K"
            context: Qt.WindowShortcut
            enabled: root.visible && d.searchable
            onActivated: settingsSections.openSearch()
        }

        StackLayout {
            anchors.fill: parent
            currentIndex: d.selectedIndex

            // 0 — Dashboard.
            DashboardView {
                buildVersion:  root.buildVersion
                portableBuild: root.portableBuild
                buildCommits:  root.buildCommits
            }

            // 1 — Apps Inspector (UI plugins).
            AppsInspectorView {
                id: appsInspector
                sourceModel: root.uiModulesModel
                loading:     root.modulesLoading
                searchText:  settingsSections.searchText

                onReloadRequested: root.appsRefreshRequested()
                onLoadRequested:   name => root.appLoadRequested(name)
                onUnloadRequested: name => root.appUnloadRequested(name)
                onVisibleChanged:  if (visible) root.appsInspectorBecameVisible()
            }

            // 2 — Module Inspector (core modules).
            ModuleInspectorView {
                id: moduleInspector
                sourceModel: root.coreModulesModel
                loading:     root.modulesLoading
                searchText:  settingsSections.searchText

                onReloadRequested: root.modulesRefreshRequested()
                onLoadRequested:   name => root.moduleLoadRequested(name)
                onUnloadRequested: name => root.moduleUnloadRequested(name)
                onVisibleChanged:  if (visible) root.moduleInspectorBecameVisible()
            }

            // 3 — Appearance (themes).
            AppearanceView {
                id: appearanceView
                onInstallRequested: file => root.themeInstallRequested(file)
                onSelectRequested:  themeId => root.themeSelectRequested(themeId)
                onRemoveRequested:  themeId => root.themeRemoveRequested(themeId)
            }

            // 4 — Package Repositories.
            RepositoriesView {
                id: repositoriesView

                repositories:   root.repositories
                loading:        root.repositoriesLoading
                downloadSource: root.downloadSource

                onRefreshRequested:    root.repositoryRefreshRequested()
                onAddRequested:        url => root.repositoryAddRequested(url)
                onRemoveRequested:     url => root.repositoryRemoveRequested(url)
                onSetEnabledRequested: (url, enabled) =>
                                           root.repositoryEnabledRequested(url, enabled)
                onDownloadSourceRequested: source => root.downloadSourceRequested(source)
                onVisibleChanged:      if (visible)
                                           root.repositoriesBecameVisible()
            }

            // 5 — Trusted Publishers.
            TrustedPublishersView {
                id: trustedPublishersView
                publishers:    root.trustedPublishers || []
                installedFrom: root.installedFromPublisher

                onRemoveRequested:  keyName => root.trustedPublisherRemoveRequested(keyName)
                onVisibleChanged:   if (visible) root.trustedPublishersRefreshRequested()
            }
        }
    }
}
