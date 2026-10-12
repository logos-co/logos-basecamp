import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

import Basecamp.Icons

Control {
    id: root

    /** Property to set the different ui modules discovered **/
    property var launcherApps: backend.launcherApps
    /** Current active section index **/
    property int currentActiveSectionIndex: backend.currentActiveSectionIndex

    signal launchUIModule(string name)
    signal updateLauncherIndex(int index)
    signal tooltipRequested(string text, real y)

    padding: 0
    bottomPadding: Theme.spacing.large
    topPadding: Theme.spacing.large + _d.systemTitleBarPadding
    topInset: _d.systemTitleBarPadding

    QtObject {
        id: _d

        readonly property var workspaceSections: [
            { name: "Workspace", icon: BasecampIcons.tents }
        ]

        // `section` is the shell section index (0 is the Workspace above).
        // Only those the host offers; a backend without the list offers all.
        readonly property var viewSections: [
            { name: "Applications",    icon: BasecampIcons.dashboard, section: 1 },
            { name: "Package Manager", icon: BasecampIcons.modules,   section: 2 },
            { name: "Settings",        icon: BasecampIcons.settings,  section: 3 }
        ].filter(function(item) {
            return !backend.availableSections || backend.availableSections.indexOf(item.section) >= 0
        })

        readonly property var loadedApps: (root.launcherApps || []).filter(function(item) {
            return item && item.isLoaded === true
        })

        readonly property var unloadedApps: (root.launcherApps || []).filter(function(item) {
            return item && item.isLoaded === false
        })

        readonly property int systemTitleBarPadding: Qt.platform.os === "osx" ? 30: 0
    }

    background: Rectangle {
        radius: Theme.spacing.radiusXlarge
        color: Theme.palette.backgroundSecondary
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing.large

        Image {
            // As per design
            Layout.preferredWidth: 46
            Layout.preferredHeight: 25
            Layout.alignment: Qt.AlignHCenter
            source: BasecampIcons.logo
        }

        SeparatorLine {}

        // Workspaces
        Column {
            Layout.fillWidth: true
            spacing: Theme.spacing.small

            Repeater {
                id: workspaceRepeater

                model: _d.workspaceSections

                SidebarIconButton {
                    required property int index
                    required property var modelData

                    width: parent.width
                    checked: root.currentActiveSectionIndex === index
                    text: modelData.name
                    icon.source: modelData.icon
                    onClicked: root.updateLauncherIndex(index)
                    onTooltipRequested: (text, y) => root.tooltipRequested(text, y)
                }
            }
        }

        SeparatorLine {}

        // Scrollable container for apps
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff

            contentItem: Flickable {
                clip: true
                contentWidth: width
                contentHeight: appsColumn.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.VerticalFlick
                interactive: contentHeight > height

                ColumnLayout {
                    id: appsColumn

                    width: parent.width
                    spacing: 2
                    
                    // Loaded apps
                    Repeater {
                        id: loadedAppsRepeater
                        model: _d.loadedApps
                        delegate: SidebarAppDelegate {
                            // Stable handle for UI automation — app tiles render
                            // icon-only (the name is a tooltip), so there is no
                            // on-screen text to click or assert on.
                            objectName: "sidebar.app." + modelData.name
                            Layout.fillWidth: true
                            loaded: true
                            loading: backend.loadingModules.indexOf(modelData.name) >= 0
                            enabled: !loading
                            checked: modelData.name === (backend.currentVisibleApp || "")
                            appName: modelData.name
                            text: modelData.displayName || modelData.name
                            iconSource: modelData.iconPath
                            fullBleedIcon: modelData.supportsFullBleedIcon === true
                            hasMissingDeps: modelData.hasMissingDeps === true
                            depBlockKind: modelData.depBlockKind || ""
                            onClicked: root.launchUIModule(modelData.name)
                            onTooltipRequested: (text, y) => root.tooltipRequested(text, y)
                        }
                    }

                    SeparatorLine {
                        Layout.topMargin: Theme.spacing.small
                        Layout.bottomMargin: Theme.spacing.small
                        visible: loadedAppsRepeater.count > 0
                    }

                    // Unloaded apps
                    Repeater {
                        model: _d.unloadedApps
                        delegate: SidebarAppDelegate {
                            objectName: "sidebar.app." + modelData.name
                            Layout.fillWidth: true
                            loaded: false
                            loading: backend.loadingModules.indexOf(modelData.name) >= 0
                            enabled: !loading
                            appName: modelData.name
                            text: modelData.displayName || modelData.name
                            iconSource: modelData.iconPath
                            fullBleedIcon: modelData.supportsFullBleedIcon === true
                            hasMissingDeps: modelData.hasMissingDeps === true
                            depBlockKind: modelData.depBlockKind || ""
                            onClicked: root.launchUIModule(modelData.name)
                            onTooltipRequested: (text, y) => root.tooltipRequested(text, y)
                        }
                    }
                }
            }
        }

        SeparatorLine {}

        // View sections (Dashboard, Modules, Settings)
        ColumnLayout {
            spacing: Theme.spacing.medium
            Layout.alignment: Qt.AlignBottom | Qt.AlignHCenter
            Repeater {
                model: _d.viewSections
                delegate: SidebarCircleButton {
                    objectName: "sidebar.section." + modelData.section
                    checked: backend.currentActiveSectionIndex === modelData.section
                    text: modelData.name
                    icon.source: modelData.icon
                    onClicked: root.updateLauncherIndex(modelData.section)
                    onTooltipRequested: (text, y) => root.tooltipRequested(text, y)
                }
            }
        }

        // Version footer — always shows the build type ("Portable" / "Dev"),
        // prefixed with the version when one is set. Selectable so the release
        // tag can be copied out of the sidebar.
        LogosSelectableText {
            objectName: "sidebar.buildLabel"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            horizontalAlignment: TextEdit.AlignHCenter
            text: {
                const base = (backend.buildVersion.length > 0 ? backend.buildVersion + " · " : "")
                    + (backend.isPortableBuild ? qsTr("Portable") : qsTr("Dev"))
                    + (backend.devHost ? " · " + qsTr("Standalone") : "")
                return backend.isMockBackend
                    ? base + " " + qsTr("(Mocked)")
                    : base
            }
            color: backend.isMockBackend ? Theme.palette.warning
                                         : Theme.palette.textSecondary
            font.pixelSize: Theme.typography.badgeText
            wrapMode: TextEdit.WrapAnywhere
        }
    }

    // Reusable component for SeparatorLine
    component SeparatorLine: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        Layout.leftMargin: Theme.spacing.tiny
        Layout.rightMargin: Theme.spacing.tiny
        color: Theme.palette.borderTertiaryMuted
    }
}
