import QtQuick
import QtQuick.Controls
import Logos.Theme

import Basecamp.Settings
import Basecamp.Backend

Item {
    id: root

    // Section indices come from ShellSection (src/ShellSections.h), the single
    // source of truth shared with MainContainer's C++ switch and
    // SidebarPanel.qml's viewSections order.
    readonly property int sidebarAppManager: ShellSection.AppManager
    readonly property int sidebarSettings:   ShellSection.Settings
    readonly property int sidebarPackages:   ShellSection.PackageManager

    Connections {
        target: backend
        function onRepositoryOperationCompleted(operation, url, success, error) {
            settingsView.reportRepositoryResult(operation, url, success, error)
        }
        function onTrustedPublisherChangeFailed(keyName, message) {
            settingsView.reportPublisherFailure(message)
        }
        // Capabilities the shell itself provides *that are pure navigation* —
        // no state, no IPC, answerable on the spot. Adding the next one of
        // those is one more `case`. Anything unrecognised must still be
        // answered, or the requester waits out the full deadline for a reply
        // never coming.
        //
        // NOT EVERY SHELL INTENT REACHES HERE. `basecamp.packages.confirm_*` is
        // intercepted in C++ before this signal is emitted — see
        // kPackageConfirmIntents in MainUIBackend.cpp. Those need the
        // cascade-unload, the dependent caches and a pending dispatch id, and
        // their dialogs live in OverlayDialogs rather than this layer. So the
        // `unavailable` default below is not the whole story: check that list
        // before concluding an intent is unhandled.
        function onShellIntentRequested(requestId, intent, params, requesterName) {
            switch (intent) {
            case "basecamp.repositories.manage":
                backend.setCurrentActiveSectionIndex(root.sidebarSettings)
                settingsView.showRepositories()
                backend.respondToShellIntent(requestId, true, ({}), "")
                return
            case "basecamp.settings.open":
                backend.setCurrentActiveSectionIndex(root.sidebarSettings)
                backend.respondToShellIntent(requestId, true, ({}), "")
                return
            case "basecamp.apps.open":
                backend.setCurrentActiveSectionIndex(root.sidebarAppManager)
                backend.respondToShellIntent(requestId, true, ({}), "")
                return
            case "basecamp.packages.open":
                backend.setCurrentActiveSectionIndex(root.sidebarPackages)
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
        color: Theme.palette.background
    }

    // Settings, the one content view left here. Apps (backend idx 0) is the
    // C++ WorkspaceArea, and Applications and Package Manager are their apps,
    // sandboxed QQuickWidgets in MainContainer's stack.
    SettingsView {
        id: settingsView
        anchors.fill: parent

        repositories:        backend.repositories
        repositoriesLoading: backend.repositoriesLoading
        downloadSource:      backend.downloadSource
        uiModulesModel:      backend.uiModulesModel
        coreModulesModel:    backend.coreModulesModel
        modulesLoading:      backend.modulesLoading
        repositoriesAvailable: backend.repositoryManagement !== false
        trustedPublishers:   backend.trustedPublishers
        installedFromPublisher: did => backend.installedFromPublisher(did)
        buildVersion:        backend.buildVersion
        portableBuild:       backend.isPortableBuild
        buildCommits:        backend.buildCommits
        userDirectory:       backend.userDirectory || ""

        onOpenUserDirectoryRequested: if (!backend.openUserDirectory())
                                          settingsView.reportOpenUserDirectoryFailed()

        onRepositoryRefreshRequested: backend.refreshCatalogs()
        onRepositoryAddRequested:     url => backend.addRepository(url)
        onRepositoryRemoveRequested:  url => backend.removeRepository(url)
        onRepositoryEnabledRequested: (url, e) => backend.setRepositoryEnabled(url, e)
        onDownloadSourceRequested:    source => backend.setDownloadSource(source)
        onRepositoriesBecameVisible: {
            Qt.callLater(backend.refreshRepositories)
            Qt.callLater(backend.refreshDownloadSource)
        }

        onTrustedPublisherRemoveRequested:   keyName => backend.removeTrustedPublisher(keyName)
        onTrustedPublishersRefreshRequested: Qt.callLater(backend.refreshTrustedPublishers)

        // Apps Inspector (UI plugins). View-only — uninstall lives in PMUI.
        onAppsRefreshRequested:       backend.refreshUiModules()
        onAppLoadRequested:           name => backend.loadUiModule(name)
        onAppUnloadRequested:         name => backend.unloadUiModule(name)
        onAppsInspectorBecameVisible: Qt.callLater(backend.refreshUiModules)

        // themeManager is set on this view only (MainContainer).
        onThemeInstallRequested: file => {
            const result = themeManager.install(file)
            if (result.ok)
                themeManager.select(result.id)
            settingsView.showThemeInstallResult(result)
        }
        onThemeSelectRequested: themeId => themeManager.select(themeId)
        onThemeRemoveRequested: themeId =>
            settingsView.showThemeRemoveResult(themeManager.remove(themeId))

        // Module Inspector (core modules). View-only — uninstall lives in PMUI.
        onModulesRefreshRequested:      backend.refreshCoreModules()
        onModuleLoadRequested:          name => backend.loadCoreModule(name)
        onModuleUnloadRequested:        name => backend.unloadCoreModule(name)
        onModuleInspectorBecameVisible: Qt.callLater(backend.refreshCoreModules)
    }
}

