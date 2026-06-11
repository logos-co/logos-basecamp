import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

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

        // Map backend index 1 → 0 (App Manager), 3 → 1 (Settings).
        currentIndex: backend.currentActiveSectionIndex === 3 ? 1 : 0

        // App Manager (backend index 1 -> internal index 0)
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
                backend.setCurrentActiveSectionIndex(3)
                settingsView.showSection(2)
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

