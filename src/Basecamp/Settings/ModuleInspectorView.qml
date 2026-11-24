import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Layouts
import Logos.Theme

import Basecamp.Backend
import Basecamp.Common

// Settings → Module Inspector. Lists every core module liblogos knows about
// with its load state and live CPU / memory stats, a Load/Unload toggle per
// row, and the per-module Interface drill-down; a tap opens a module's detail
// beside the table, or as the page on a phone. Uninstall is deliberately
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

    readonly property bool canGoBack: showingInterface || details.showingPanel
    readonly property string backTitle: showingInterface && details.showingPanel && d.openRow
                                        ? d.openRow.label : qsTr("Module Inspector")

    function goBack() {
        if (showingInterface)
            showingInterface = false
        else
            details.panelOpen = false
    }

    property string selectedPlugin: ""
    property bool showingInterface: false

    // Core modules Basecamp itself runs on: the package pipeline
    // (package_manager, package_downloader) and the auth-token registry
    // (capability_module). Their Load/Unload toggle renders disabled — unloading
    // any of them from the inspector would take the app's own plumbing down.
    readonly property var protectedModules: ["package_manager", "package_downloader", "capability_module"]

    // Open a specific module's Interface screen (methods + events) by name.
    // Equivalent to clicking that module's "Interface" button — exposed for UI
    // automation/tests, which can't disambiguate the per-row buttons by their
    // identical label.
    function openInterface(name) {
        selectedPlugin = name;
        showingInterface = true;
    }

    // Opens a module's detail, as a tap on its row does.
    function openDetails(name) {
        d.openName = name
        details.panelOpen = true
    }

    QtObject {
        id: d

        // The module whose detail is open, found by name among all the rows,
        // not the shown ones: a search that hides its row leaves the detail
        // open, and the stats poll updates it in place.
        property string openName: ""
        property int revision: 0
        readonly property var openRow: {
            revision
            for (let i = 0; i < allRows.count; ++i) {
                const entry = allRows.objectAt(i)
                if (entry && entry.row.name === openName)
                    return entry.row
            }
            return null
        }
        readonly property bool openLocked: root.protectedModules.indexOf(openName) !== -1

        // A module that is gone closes its detail, once a reset of the
        // model has put the others back.
        onOpenRowChanged: if (!openRow) Qt.callLater(closeIfGone)
        function closeIfGone() {
            if (!openRow)
                details.panelOpen = false
        }
    }

    Instantiator {
        id: allRows
        model: root.sourceModel
        delegate: QtObject { readonly property var row: model }
        onObjectAdded: d.revision++
        onObjectRemoved: d.revision++
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

        // ─── 0: module list, and a module's detail ───
        LogosSidePanel {
            id: details
            objectName: "moduleInspector.details"

            title: d.openRow ? d.openRow.label : ""
            backInHost: true
            onPanelOpenChanged: if (!panelOpen) d.openName = ""

            panelActions: [
                LogosButton {
                    objectName: "moduleInspector.details.loadToggle"
                    visible: !!d.openRow && !d.openRow.isMainUi
                    enabled: !root.loading && !d.openLocked
                    text: d.openRow && d.openRow.isLoaded ? qsTr("Unload") : qsTr("Load")
                    variant: d.openRow && d.openRow.isLoaded ? LogosButton.Variant.Secondary
                                                             : LogosButton.Variant.Primary
                    onClicked: {
                        if (d.openRow.isLoaded) root.unloadRequested(d.openName)
                        else                    root.loadRequested(d.openName)
                    }
                },
                LogosButton {
                    objectName: "moduleInspector.details.interface"
                    visible: !!d.openRow && d.openRow.isLoaded
                    text: qsTr("Interface")
                    onClicked: root.openInterface(d.openName)
                }
            ]

            panel: ModuleDetail {
                row: d.openRow
                label: d.openRow ? d.openRow.label : ""
                name: d.openRow ? d.openRow.name : ""
                version: d.openRow ? d.openRow.version || "" : ""
                description: d.openRow ? d.openRow.description || "" : ""
                fields: !d.openRow ? [] : [
                    [qsTr("Version"), d.openRow.version || ""],
                    [qsTr("CPU"), d.openRow.isLoaded ? Number(d.openRow.cpu).toFixed(1) + "%" : ""],
                    [qsTr("Memory"), d.openRow.isLoaded ? Number(d.openRow.memory).toFixed(1) + " MB" : ""]
                ]
            }

            content: ColumnLayout {
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
                    phoneCards: true
                    rowsOpen: true
                    rowMenu: showingCards ? phoneRowMenu : null
                    onRowClicked: (index, row) => root.openDetails(row.name)

                    ModuleRowMenu {
                        id: phoneRowMenu
                        row: modulesTable.menuRow
                        busy: root.loading
                        locked: !!row && root.protectedModules.indexOf(row.name) !== -1
                        interfaceEnabled: true

                        onOpenRequested: root.openDetails(row.name)
                        onLoadToggleRequested: {
                            if (row.isLoaded) root.unloadRequested(row.name)
                            else              root.loadRequested(row.name)
                        }
                        onInterfaceRequested: root.openInterface(row.name)
                    }

                    columns: [
                        LogosTableColumn {
                            title: qsTr("Module")
                            role: "label"
                            minWidth: 180
                            preferredWidth: 240
                            fillWidth: true
                            sortable: true
                            cellDelegate: moduleCellComponent
                        },
                        LogosTableColumn {
                            title: qsTr("Status")
                            role: "statusText"
                            minWidth: 130
                            preferredWidth: 130
                            sortable: true
                            card: "badge"
                            cellDelegate: statusCellComponent
                        },
                        LogosTableColumn {
                            title: qsTr("CPU")
                            role: "cpu"
                            minWidth: 90
                            preferredWidth: 90
                            priority: 2
                            sortable: true
                            card: "hidden"
                            cellDelegate: cpuCellComponent
                        },
                        LogosTableColumn {
                            title: qsTr("Memory")
                            role: "memory"
                            minWidth: 110
                            preferredWidth: 110
                            priority: 1
                            sortable: true
                            card: "hidden"
                            cellDelegate: memoryCellComponent
                        },
                        LogosTableColumn {
                            title: ""
                            minWidth: 240
                            preferredWidth: 240
                            alignment: Qt.AlignRight | Qt.AlignVCenter
                            card: "hidden"
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

                        // Sized by its badge, so a card can show it too.
                        Item {
                            implicitWidth: statusBadge.implicitWidth
                            implicitHeight: statusBadge.implicitHeight

                            ModuleStatusBadge {
                                id: statusBadge
                                // Automation-only: per-module handle so UI tests
                                // can assert one row's load state — the badge
                                // wording ("Loaded"/"Not loaded") also appears in
                                // other tables, so text matching is ambiguous.
                                objectName: "moduleInspector.status."
                                            + (rowItem && rowItem.name ? rowItem.name : "")
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
                                locked: !!rowItem && root.protectedModules.indexOf(rowItem.name) !== -1
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
        }

        // ─── 1: Interface (methods + events) ───
        Item {
            // Its way back is Settings' back row.
            PluginInterfaceView {
                anchors.fill: parent
                anchors.margins: Theme.spacing.large
                pluginName: root.selectedPlugin
            }
        }
    }

    // Same Reload feedback as the App Manager / Package Manager panels.
    LoadingOverlay {
        anchors.fill: parent
        visible: root.loading
    }
}
