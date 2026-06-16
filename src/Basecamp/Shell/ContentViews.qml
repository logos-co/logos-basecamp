import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Basecamp.AppManager
import Basecamp.Settings

Item {
    id: root

    // Sidebar section indices (backend's m_sections list, defined by
    // SidebarPanel.qml). 0 = Apps (MdiView), 1 = App Manager,
    // 2 = Modules, 3 = Settings.
    readonly property int sidebarAppManager: 1
    readonly property int sidebarSettings:   3

    Connections {
        target: backend
        function onRepositoryOperationCompleted(operation, url, success, error) {
            settingsView.reportRepositoryResult(operation, url, success, error)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#1e1e1e"
    }

    // Content views stack — only App Manager and Settings now live here.
    // Apps (backend idx 0) is the C++ MdiView, and Modules (backend idx 2)
    // is the sandboxed package_manager_ui QQuickWidgetxwx
    StackLayout {
        id: contentStack
        anchors.fill: parent

        // Map backend's sidebar index to this stack's two-entry layout.
        currentIndex: backend.currentActiveSectionIndex === root.sidebarSettings ? 1 : 0

        // App Manager (sidebar sidebarAppManager -> stack index 0)
        AppManagerView {
            id: appManagerView
            appsProxy:    backend.uiAppsProxy
            repositories: backend.repositories
            loading:      backend.appsLoading
            onAppClicked: function(name, repositoryUrl) {
                // Primary click — fast-path launch for installed apps.
                backend.openApp(name, repositoryUrl, ({}), true)
            }
            onManageAppRequested: function(name, repositoryUrl) {
                // Right-click / long-press — force the dialog open.
                backend.openApp(name, repositoryUrl, ({}), false)
            }
            onNavigateToRepositories: {
                backend.setCurrentActiveSectionIndex(root.sidebarSettings)
                settingsView.showRepositories()
            }
        }

        // Settings (backend index 3 -> internal index 1)
        SettingsView {
            id: settingsView

            repositories:        backend.repositories
            repositoriesLoading: backend.repositoriesLoading

            onRepositoryRefreshRequested: backend.refreshRepositories()
            onRepositoryAddRequested:     url => backend.addRepository(url)
            onRepositoryRemoveRequested:  url => backend.removeRepository(url)
            onRepositoryEnabledRequested: (url, e) => backend.setRepositoryEnabled(url, e)
            onRepositoriesBecameVisible: Qt.callLater(backend.refreshRepositories)
        }
    }
}

