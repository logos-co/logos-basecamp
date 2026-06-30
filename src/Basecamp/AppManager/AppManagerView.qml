import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Theme

import Basecamp.Backend

Rectangle {
    id: root

    // ─── Public API ───
    property var appsProxy: null
    property var repositoriesModel: null
    property bool loading: false
    signal appClicked(string name, string repositoryUrl)
    signal manageAppRequested(string name, string repositoryUrl)
    signal navigateToRepositories()
    signal refreshRequested()

    QtObject {
        id: d

        readonly property var stateTabs: [
            { label: qsTr("All"),           filter: "all" },
            { label: qsTr("Installed"),     filter: "installed" },
            { label: qsTr("Not Installed"), filter: "notInstalled" },
        ]

        readonly property string installStateFilter:
            (d.stateTabs[stateTabBar.currentIndex]).filter || "all"
        onInstallStateFilterChanged:
            if (root.appsProxy) root.appsProxy.installStateFilter = installStateFilter

        property string searchText: ""
        onSearchTextChanged:
            if (root.appsProxy) root.appsProxy.searchText = searchText

        property int    selectedCategoryIndex: 0   // 0 = "All"
        property string viewMode: "grid"

        readonly property var categories:
            root.appsProxy ? root.appsProxy.categories : ["All"]

        readonly property string selectedCategory:
            d.categories[d.selectedCategoryIndex] || "All"
        onSelectedCategoryChanged:
            if (root.appsProxy) root.appsProxy.categoryFilter = selectedCategory
    }

    // Initial sync — push d's current values onto the proxy as soon as
    // it's assigned. The change handlers inside `d` keep things in sync
    // after that.
    onAppsProxyChanged: {
        if (appsProxy) {
            appsProxy.categoryFilter     = d.selectedCategory
            appsProxy.searchText         = d.searchText
            appsProxy.installStateFilter = d.installStateFilter
        }
    }

    color: Theme.palette.background

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.xxlarge
        spacing: Theme.spacing.xlarge

        // Header: title + subtitle on the left, search on the right.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.xlarge

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.tiny

                LogosText {
                    text: "Applications"
                    font.pixelSize: Theme.typography.pageTitleText
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                }

                LogosText {
                    text: "Install and manage applications."
                    font.pixelSize: Theme.typography.primaryText
                    color: Theme.palette.textSecondary
                }
            }

            Item { Layout.fillWidth: true }

            LogosSearchBar {
                id: searchBar
                Layout.alignment: Qt.AlignRight
                Layout.preferredWidth: 480
                Layout.minimumWidth: 200
                text: d.searchText
                placeholderText: qsTr("Search apps…")
                shortcutHint: "⌘K"
                onTextChanged: {
                    if (text !== d.searchText)
                        d.searchText = text
                }
            }

            // ⌘K focuses + selects the search bar.
            Shortcut {
                sequence: "Ctrl+K"
                context: Qt.WindowShortcut
                onActivated: {
                    searchBar.textInput.forceActiveFocus()
                    searchBar.textInput.selectAll()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing.xlarge

            // ─── Categories sidebar ───
            ColumnLayout {
                Layout.preferredWidth: 200
                Layout.minimumWidth: 160
                Layout.maximumWidth: 200
                Layout.fillHeight: true
                spacing: Theme.spacing.tiny

                LogosText {
                    Layout.topMargin: Theme.spacing.tiny
                    Layout.bottomMargin: Theme.spacing.tiny
                    text: qsTr("Categories")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightRegular
                    color: Theme.palette.text
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: contentHeight
                    interactive: false
                    spacing: Theme.spacing.tiny
                    model: d.categories
                    currentIndex: d.selectedCategoryIndex

                    delegate: LogosItemDelegate {
                        width: ListView.view.width
                        text: modelData
                        highlighted: ListView.isCurrentItem
                        radius: Theme.spacing.radiusLarge
                        highlightColor: Theme.palette.backgroundButton
                        hoverColor: "transparent"
                        textColor: (highlighted || hovered)
                                       ? Theme.palette.text
                                       : Theme.palette.textTertiary
                        onClicked: d.selectedCategoryIndex = index
                    }
                }

                Item { Layout.fillWidth: true; Layout.fillHeight: true }
            }

            // ─── Apps panel ───
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.palette.surfaceRaised
                radius: Theme.spacing.radiusXlarge
                clip: true

                LogosSpinner {
                    anchors.centerIn: parent
                    visible: root.loading
                    running: root.loading
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.large
                    spacing: Theme.spacing.medium
                    visible: !root.loading

                    // ─── Panel header: title + install-state tabs + view toggle ───
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing.xlarge

                        LogosText {
                            Layout.preferredHeight: implicitHeight
                            text: qsTr("Apps")
                            font.pixelSize: Theme.typography.panelTitleText
                            font.weight: Theme.typography.weightMedium
                            color: Theme.palette.text
                        }

                        LogosTabBar {
                            id: stateTabBar

                            Layout.preferredWidth: implicitWidth
                            spacing: Theme.spacing.large

                            Repeater {
                                model: d.stateTabs
                                LogosTabButton {
                                    required property var modelData
                                    text: modelData.label
                                }
                            }
                        }

                        Item { Layout.fillWidth: true }

                        LogosIconButton {
                            iconSource: LogosIcons.refresh
                            size: 36
                            iconSize: 18
                            iconColor: Theme.palette.textTertiary
                            background: Rectangle {
                                radius: Theme.spacing.radiusLarge
                                color: parent.hovered ? Theme.palette.backgroundButton
                                                      : "transparent"
                            }
                            ToolTip.text: qsTr("Reload apps")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: root.refreshRequested()
                        }

                        LogosButton {
                            Layout.minimumWidth: 100
                            Layout.preferredWidth: 130
                            Layout.maximumWidth: 130
                            Layout.preferredHeight: 40
                            radius: Theme.spacing.radiusLarge
                            text: qsTr("Repositories")
                            onClicked: root.navigateToRepositories()
                        }

                        RowLayout {
                            spacing: 24
                            Layout.preferredHeight: 36

                            LogosText {
                                Layout.alignment: Qt.AlignVCenter
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("View:")
                                color:  Theme.palette.textTertiary
                            }

                            LogosIconButton {
                                iconSource: LogosIcons.grid
                                size: 20
                                iconSize: 20
                                iconColor: d.viewMode === "grid"
                                           ? Theme.palette.text
                                           : Theme.palette.textTertiary
                                onClicked: d.viewMode = "grid"
                                background: Item{}
                            }

                            LogosIconButton {
                                iconSource: LogosIcons.list
                                size: 20
                                iconSize: 20
                                iconColor: d.viewMode === "list"
                                           ? Theme.palette.text
                                           : Theme.palette.textTertiary
                                onClicked: d.viewMode = "list"
                                background: Item{}
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: Theme.palette.borderSubtle
                    }

                    Flickable {
                        id: gridScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: width
                        contentHeight: gridColumn.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds

                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                        }

                        ColumnLayout {
                            id: gridColumn
                            width: gridScroll.width
                            spacing: Theme.spacing.large

                            Repeater {
                                model: root.repositoriesModel
                                delegate: ColumnLayout {
                                    required property int index
                                    required property string url
                                    required property string displayName
                                    required property string name
                                    required property bool isDefault
                                    required property bool enabled

                                    ModuleFilterProxy {
                                        id: repoFilter
                                        sourceModel: root.appsProxy
                                        repositoryUrlFilter: url || ""
                                        excludeMainUi: false
                                    }

                                    Layout.fillWidth: true
                                    spacing: Theme.spacing.medium
                                    visible: enabled !== false
                                             && repoFilter.visibleCount > 0

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacing.small

                                        LogosText {
                                            text: isDefault === true
                                                ? qsTr("Starter Apps")
                                                : (displayName
                                                   || name
                                                   || url
                                                   || qsTr("Repository"))
                                            font.pixelSize: Theme.typography.subtitleText
                                            font.weight: Theme.typography.weightMedium
                                            color: Theme.palette.textSecondary
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        LogosText {
                                            text: "(" + repoFilter.visibleCount + ")"
                                            font.pixelSize: Theme.typography.secondaryText
                                            color: Theme.palette.textTertiary
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: Theme.palette.borderSubtle
                                        opacity: 0.5
                                    }

                                    AppGrid {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: implicitHeight
                                        modulesSource: repoFilter
                                        viewMode: d.viewMode
                                        onAppClicked: function(name, repositoryUrl) {
                                            root.appClicked(name, repositoryUrl || "")
                                        }
                                        onAppManageRequested: function(name, repositoryUrl) {
                                            root.manageAppRequested(name, repositoryUrl || "")
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
