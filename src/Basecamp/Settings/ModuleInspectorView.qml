import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Theme

import Basecamp.Backend
import Basecamp.Common

// Settings → Module Inspector. Lists every core module liblogos knows about
// with its load state and live CPU / memory stats, a Load/Unload toggle per
// row, and the per-module Interface drill-down. Uninstall is deliberately
// absent — module management lives in the Package Manager; this view is
// read-only apart from the load toggle.
//
// Formerly the "Core Modules" tab of ModulesView. Split out into its own
// Settings section alongside Apps Inspector, restyled onto LogosTable +
// InspectorPanelHeader to match the App Manager and Package Manager panels.
Item {
    id: root
    objectName: "moduleInspectorView"

    // ─── Public API ───
    // View emits, ContentViews calls the backend — see the Settings wiring in
    // Basecamp/Shell/ContentViews.qml. The Interface screen is the exception:
    // PluginInterfaceView still queries the backend for methods/events itself.
    // `searchText` is owned by SettingsView (one shared page-level search bar,
    // styled like AppManagerView).
    property var sourceModel: null
    property bool loading: false
    property string searchText: ""

    signal reloadRequested()
    signal loadRequested(string name)
    signal unloadRequested(string name)

    property string selectedPlugin: ""
    property bool showingInterface: false

    // Open a specific module's Interface screen (methods + events) by name.
    // Equivalent to clicking that module's "Interface" button — exposed for UI
    // automation/tests, which can't disambiguate the per-row buttons by their
    // identical label.
    function openInterface(name) {
        selectedPlugin = name;
        showingInterface = true;
    }

    ModulesFilterProxy {
        id: tableModel

        sourceModel:  root.sourceModel
        searchText:   root.searchText
        sortRoleName: modulesTable.sortRole
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: root.showingInterface ? 1 : 0

        // ─── 0: module list ───
        // Vertical insets are Layout margins on the children rather than
        // anchors on this ColumnLayout — StackLayout owns its children's
        // geometry, so anchor margins here would be ignored.
        ColumnLayout {
            spacing: Theme.spacing.medium

            InspectorPanelHeader {
                id: header

                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacing.large
                Layout.rightMargin: Theme.spacing.large
                Layout.topMargin: Theme.spacing.large
                Layout.minimumWidth: 0

                title: qsTr("Module Inspector")
                subtitle: qsTr("Core modules known to the runtime, with live resource usage.")
                reloadObjectName: "moduleInspector.reloadButton"
                loading: root.loading
                visibleCount: tableModel.visibleCount
                totalCount: tableModel.totalCount

                onReloadClicked: root.reloadRequested()
            }

            LogosTable {
                id: modulesTable
                objectName: "moduleInspector.table"

                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.bottomMargin: Theme.spacing.large

                model: tableModel
                rowHeight: 56
                sortRole: "label"
                sortOrder: Qt.AscendingOrder
                emptyText: tableModel.totalCount === 0
                           ? qsTr("No core modules available.")
                           : qsTr("No modules match the current filter.")

                onSortRequested: function(role, order) {
                    modulesTable.sortRole = role
                    modulesTable.sortOrder = order
                    tableModel.applySortOrder(order)
                }

                // Keep every row instantiated — see AppsInspectorView for why.
                Component.onCompleted: if (view) view.cacheBuffer = 20000

                columns: [
                    LogosTableColumn {
                        title: qsTr("Module")
                        role: "label"
                        minWidth: 180
                        preferredWidth: 240
                        sortable: true
                        cellDelegate: moduleCellComponent
                    },
                    LogosTableColumn {
                        title: qsTr("Status")
                        role: "statusText"
                        minWidth: 110
                        preferredWidth: 130
                        sortable: true
                        cellDelegate: statusCellComponent
                    },
                    LogosTableColumn {
                        title: qsTr("CPU")
                        role: "cpu"
                        minWidth: 80
                        preferredWidth: 90
                        sortable: true
                        cellDelegate: cpuCellComponent
                    },
                    LogosTableColumn {
                        title: qsTr("Memory")
                        role: "memory"
                        minWidth: 90
                        preferredWidth: 110
                        sortable: true
                        cellDelegate: memoryCellComponent
                    },
                    LogosTableColumn {
                        title: ""
                        minWidth: 240
                        preferredWidth: 260
                        fillWidth: true
                        alignment: Qt.AlignRight | Qt.AlignVCenter
                        cellDelegate: actionsCellComponent
                    }
                ]

                Component {
                    id: moduleCellComponent

                    ColumnLayout {
                        spacing: 0

                        LogosText {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            text: rowItem ? rowItem.label : ""
                            font.pixelSize: Theme.typography.primaryText
                            font.weight: Theme.typography.weightMedium
                            color: Theme.palette.text
                            elide: Text.ElideRight
                        }

                        // Raw module name — the identifier other modules call
                        // it by. Hidden when it matches the display name.
                        LogosText {
                            Layout.fillWidth: true
                            visible: rowItem && rowItem.label !== rowItem.name
                            text: rowItem ? rowItem.name : ""
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textTertiary
                            elide: Text.ElideRight
                        }
                    }
                }

                Component {
                    id: statusCellComponent

                    Item {
                        ModuleStatusBadge {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            row: rowItem
                        }
                    }
                }

                // Stats only mean something for a running module; unloaded rows
                // render an em dash so the column stays aligned without
                // implying "0% CPU" is a measurement.
                Component {
                    id: cpuCellComponent

                    LogosText {
                        text: (rowItem && rowItem.isLoaded)
                              ? Number(rowItem.cpu).toFixed(1) + "%" : "—"
                        color: (rowItem && rowItem.isLoaded) ? Theme.palette.text
                                                            : Theme.palette.textMuted
                        font.pixelSize: Theme.typography.primaryText
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                }

                Component {
                    id: memoryCellComponent

                    LogosText {
                        text: (rowItem && rowItem.isLoaded)
                              ? Number(rowItem.memory).toFixed(1) + " MB" : "—"
                        color: (rowItem && rowItem.isLoaded) ? Theme.palette.text
                                                            : Theme.palette.textMuted
                        font.pixelSize: Theme.typography.primaryText
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                }

                Component {
                    id: actionsCellComponent

                    Item {
                        ModuleRowActions {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            row: rowItem
                            busy: root.loading
                            interfaceEnabled: true

                            onLoadToggleRequested: {
                                if (rowItem.isLoaded) root.unloadRequested(rowItem.name)
                                else                 root.loadRequested(rowItem.name)
                            }
                            onInterfaceRequested: root.openInterface(rowItem.name)
                        }
                    }
                }
            }
        }

        // ─── 1: Interface (methods + events) ───
        Item {
            PluginInterfaceView {
                anchors.fill: parent
                anchors.margins: Theme.spacing.large
                pluginName: root.selectedPlugin
                onBackClicked: root.showingInterface = false
            }
        }
    }

    // Same Reload feedback as the App Manager / Package Manager panels.
    LoadingOverlay {
        anchors.fill: parent
        visible: root.loading
    }
}
