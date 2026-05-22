import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import controls

Control {
    id: root

    /** All sidebar sections (workspaces + views) **/
    property var sections: backend.sections
    /** Property to set the different ui modules discovered **/
    property var launcherApps: backend.launcherApps
    /** Current active section index **/
    property int currentActiveSectionIndex: backend.currentActiveSectionIndex

    signal launchUIModule(string name)
    signal updateLauncherIndex(int index)

    padding: 0
    bottomPadding: Theme.spacing.large
    topPadding: Theme.spacing.large + _d.systemTitleBarPadding
    topInset: _d.systemTitleBarPadding

    QtObject {
        id: _d

        // Filter sections by type
        readonly property var workspaceSections: (root.sections || []).filter(function(item) {
            return item && item.type === "workspace"
        })

        readonly property var viewSections: (root.sections || []).filter(function(item) {
            return item && item.type === "view"
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
            source: "qrc:/icons/basecamp.png"
        }

        // Pre-release version badge — only shown for builds whose version
        // string carries a pre-release suffix (-rc, -alpha, -beta, -dev).
        // Stable releases show nothing. Keeps screenshots and bug reports
        // self-documenting without cluttering the production UI.
        Text {
            visible: /(-rc|-alpha|-beta|-dev)/i.test(backend.buildVersion)
            text: backend.buildVersion
            color: "#4B5563"
            font.pixelSize: 10
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: -Theme.spacing.large + 2
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
                    icon.source: modelData.iconPath
                    onClicked: root.updateLauncherIndex(index)
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
                            Layout.fillWidth: true
                            loaded: true
                            loading: backend.loadingModules.indexOf(modelData.name) >= 0
                            enabled: !loading
                            checked: modelData.name === (backend.currentVisibleApp || "")
                            text: modelData.name
                            icon.source: modelData.iconPath
                            hasMissingDeps: modelData.hasMissingDeps === true
                            onClicked: root.launchUIModule(modelData.name)
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
                            Layout.fillWidth: true
                            loaded: false
                            loading: backend.loadingModules.indexOf(modelData.name) >= 0
                            enabled: !loading
                            text: modelData.name
                            icon.source: modelData.iconPath
                            hasMissingDeps: modelData.hasMissingDeps === true
                            onClicked: root.launchUIModule(modelData.name)
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
                    checked: backend.currentActiveSectionIndex -1 === index
                    text: modelData.name
                    icon.source: modelData.iconPath
                    onClicked: root.updateLauncherIndex(_d.workspaceSections.length + index)
                }
            }
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
