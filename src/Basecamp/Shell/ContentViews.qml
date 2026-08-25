import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Basecamp.AppManager
import Basecamp.Settings
import Basecamp.Backend

Item {
    id: root

    // Section indices come from ShellSection (src/ShellSections.h), the single
    // source of truth shared with MainContainer's C++ switch and
    // SidebarPanel.qml's viewSections order.
    readonly property int sidebarAppManager: ShellSection.AppManager
    readonly property int sidebarSettings:   ShellSection.Settings

    // The App Manager's view of the catalog. Declared here rather than handed
    // over by the backend: a filter proxy is view configuration, so it belongs
    // to whoever draws the view. Only the source model crosses, as a plain
    // QAbstractItemModel*.
    AppsFilterProxy {
        id: uiAppsProxy
        sourceModel:   backend.appsModel
        typeFilter:    "ui_qml"
        excludeMainUi: true
    }

    Connections {
        target: backend
        function onRepositoryOperationCompleted(operation, url, success, error) {
            settingsView.reportRepositoryResult(operation, url, success, error)
        }
        // Capabilities the shell itself provides. Adding the next one is one
        // more `case`. Anything unrecognised must still be answered, or the
        // requester waits out the full deadline for a reply never coming.
        function onShellIntentRequested(requestId, intent, params, requesterName) {
            switch (intent) {
            case "logos.repositories.manage":
                backend.setCurrentActiveSectionIndex(root.sidebarSettings)
                settingsView.showRepositories()
                backend.respondToShellIntent(requestId, true, ({}), "")
                return
            }
            console.warn("ContentViews: unhandled shell intent", intent,
                         "from", requesterName)
            backend.respondToShellIntent(requestId, false, ({}), "unavailable")
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#1e1e1e"
    }

    // Content views stack — only App Manager and Settings live here.
    // Apps (backend idx 0) is the C++ WorkspaceArea (QDockWidget-based),
    // and Modules (backend idx 2) is the sandboxed package_manager_ui
    // QQuickWidget.
    StackLayout {
        id: contentStack
        anchors.fill: parent

        // Map backend's sidebar index to this stack's two-entry layout.
        currentIndex: backend.currentActiveSectionIndex === root.sidebarSettings ? 1 : 0

        // App Manager (sidebar sidebarAppManager -> stack index 0)
        AppManagerView {
            id: appManagerView
            appsProxy:      uiAppsProxy
            repositories:   backend.repositories
            loading:        backend.appsLoading
            onAppClicked: function(name, repositoryUrl) {
                // Primary click — fast-path launch for installed apps.
                backend.openApp(name, repositoryUrl, ({}), true)
            }
            onManageAppRequested: function(name, repositoryUrl) {
                // Context menu's Install… / App details… — force the dialog
                // open. The modal is the install confirmation (version picker
                // + required packages), so both items land here.
                backend.openApp(name, repositoryUrl, ({}), false)
            }
            onUninstallAppRequested: function(name, repositoryUrl) {
                backend.uninstallApp(name, repositoryUrl)
            }
            onNavigateToRepositories: {
                backend.setCurrentActiveSectionIndex(root.sidebarSettings)
                settingsView.showRepositories()
            }
            onRefreshRequested: backend.refreshAppCatalog()
        }

        // Settings (backend index 3 -> internal index 1)
        SettingsView {
            id: settingsView

            repositories:        backend.repositories
            repositoriesLoading: backend.repositoriesLoading
            uiModulesModel:      backend.uiModulesModel
            coreModulesModel:    backend.coreModulesModel
            modulesLoading:      backend.modulesLoading

            onRepositoryRefreshRequested: backend.refreshRepositories()
            onRepositoryAddRequested:     url => backend.addRepository(url)
            onRepositoryRemoveRequested:  url => backend.removeRepository(url)
            onRepositoryEnabledRequested: (url, e) => backend.setRepositoryEnabled(url, e)
            onRepositoriesBecameVisible: Qt.callLater(backend.refreshRepositories)

            // Apps Inspector (UI plugins). View-only — uninstall lives in PMUI.
            onAppsRefreshRequested:       backend.refreshUiModules()
            onAppLoadRequested:           name => backend.loadUiModule(name)
            onAppUnloadRequested:         name => backend.unloadUiModule(name)
            onAppsInspectorBecameVisible: Qt.callLater(backend.refreshUiModules)

            // Module Inspector (core modules). View-only — uninstall lives in PMUI.
            onModulesRefreshRequested:      backend.refreshCoreModules()
            onModuleLoadRequested:          name => backend.loadCoreModule(name)
            onModuleUnloadRequested:        name => backend.unloadCoreModule(name)
            onModuleInspectorBecameVisible: Qt.callLater(backend.refreshCoreModules)
        }
    }
}

