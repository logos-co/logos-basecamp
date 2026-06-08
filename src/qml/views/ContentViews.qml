import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    Rectangle {
        anchors.fill: parent
        color: "#1e1e1e"
    }

    // Content views stack (indices 1-4 from backend, mapped to 0-3 here)
    // Index 0 (Apps/MDI) is handled by the C++ MdiView widget
    StackLayout {
        id: contentStack
        anchors.fill: parent

        // Map backend index: 1=Dashboard, 2=Modules, 3=Settings, 4=App Manager
        // to internal index: 0=Dashboard, 1=Modules, 2=Settings, 3=App Manager
        currentIndex: Math.max(0, backend.currentActiveSectionIndex - 1)

        // Dashboard (backend index 1 -> internal index 0)
        DashboardView {
            id: dashboardView
        }

        // Modules (backend index 2 -> internal index 1)
        ModulesView {
            id: modulesView
            onVisibleChanged: if (visible) {
                backend.refreshUiModules()
                backend.refreshCoreModules()
            }
        }

        // Settings (backend index 3 -> internal index 2)
        SettingsView {
            id: settingsView
        }

        // App Manager (backend index 4 -> internal index 3)
        AppManagerView {
            id: appManagerView
            appsProxy: backend.uiAppsProxy
            onAppClicked: function(name, repositoryUrl) {
                // Primary click — fast-path launch for installed apps.
                backend.openApp(name, repositoryUrl, ({}), true)
            }
            onManageAppRequested: function(name, repositoryUrl) {
                // Right-click / long-press — force the dialog open.
                backend.openApp(name, repositoryUrl, ({}), false)
            }
        }
    }
}

