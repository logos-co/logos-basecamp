import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    Rectangle {
        anchors.fill: parent
        color: "#1e1e1e"
    }

    // Content views stack (backend indices 1-3, mapped to 0-2 here).
    // Index 0 (Apps/MDI) is handled by the C++ MdiView widget.
    StackLayout {
        id: contentStack
        anchors.fill: parent

        // Map backend index: 1=App Manager, 2=Modules (placeholder), 3=Settings
        // to internal index: 0=App Manager, 1=placeholder, 2=Settings
        currentIndex: Math.max(0, backend.currentActiveSectionIndex - 1)

        // App Manager (backend index 1 -> internal index 0)
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

        // Place holder for PMUI
        Item {
            id: modulesPlaceholder
        }

        // Settings (backend index 3 -> internal index 2)
        SettingsView {
            id: settingsView
        }
    }
}

