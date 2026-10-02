import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

import Basecamp.AppManager
import Basecamp.Backend 1.0

import Basecamp.Icons

Item {
    id: root

    signal discoverApplicationsClicked()
    signal managePackagesClicked()
    signal reopenAppRequested(string moduleName)
    signal appActivated(string moduleName, string repositoryUrl)
    signal packageActivated(string moduleName)
    signal packageInstallRequested(string moduleName)
    signal showAllResultsRequested(string typeValue, string query)
    function clearSearch() {
        searchField.text = ""
        d.typeFilter = ""
    }

    // `sequence`, not `sequences`: Qt 6.9's nativeText reads only `sequence`.
    Shortcut {
        id: searchShortcut
        objectName: "welcomePage.searchShortcut"
        sequence: "Ctrl+K"
        context: Qt.WindowShortcut
        enabled: root.visible
        onActivated: searchField.forceActiveFocus()
    }

    QtObject {
        id: d
        readonly property bool hasApps:
            typeof backend !== "undefined" && backend !== null &&
            (backend.launcherApps || []).length > 0

        readonly property int maxRecent: 5
        readonly property var recentApps:
            typeof backend !== "undefined" && backend !== null
                ? (backend.recentlyClosedApps || []).slice(0, maxRecent)
                : []

        readonly property int columnWidth: 992
        readonly property int sectionSpacing: 64
        readonly property int headerGap: 150
        readonly property int cardGap: 24
        readonly property int greetingSize: 40
        readonly property int brandGap: 64
        readonly property int sectionGap: 42

        readonly property int tileSize: 120
        readonly property int cellWidth: tileSize
        readonly property int cellHeight: tileSize + Theme.spacing.medium + 28
        readonly property int tileGap: Theme.spacing.xxlarge
        readonly property int resultTileSize: 80
        readonly property int resultCellWidth: resultTileSize
        readonly property int resultCellHeight: resultTileSize
                                                + Theme.spacing.medium + 28
        readonly property int resultGap: Theme.spacing.xxlarge
        readonly property int rowIconSize: 24
        readonly property int resultCapacity:
            Math.max(1, Math.floor((column.width - rowIconSize)
                                   / (resultCellWidth + resultGap)))

        // ─── Search ───
        property string typeFilter: ""
        readonly property string query: searchField.text.trim()
        readonly property bool searching: query.length > 0

        // Results come from the catalog, which is fetched the first time
        // something needs it: the first query here, unless something else was.
        onSearchingChanged: if (searching && typeof backend !== "undefined" && backend !== null)
                                backend.requestCatalog()
        readonly property bool catalogLoading:
            typeof backend !== "undefined" && backend !== null && backend.appsLoading
        readonly property bool noMatches:
            (typeFilter !== "core" ? appsProxy.visibleCount : 0)
            + (typeFilter !== "ui_qml" ? packagesProxy.visibleCount : 0) === 0

        readonly property int greetingToSearch: 40
        readonly property int searchFieldHeight: 54
        readonly property int filterButtonSize: 50
        readonly property int filterIconSize: 28
        readonly property int searchRowGap: 21
        readonly property real cardScale: 1.0

        // Below this the brand and the two cards stop fitting side by side.
        readonly property bool narrow: root.width < 900
    }

    implicitWidth: 1000

    LogosScrollView {
        id: scroller

        anchors.fill: parent

        Item {
            width: scroller.availableWidth
            height: Math.max(scroller.availableHeight,
                             column.implicitHeight + 2 * Theme.spacing.xxlarge)

            ColumnLayout {
                id: column

                anchors.centerIn: parent
                width: Math.min(headerRow.implicitWidth,
                                d.columnWidth,
                                scroller.availableWidth - 2 * Theme.spacing.xxlarge)
                spacing: d.sectionSpacing

                GridLayout {
                    id: headerRow

                    Layout.fillWidth: true
                    columns: d.narrow ? 1 : 2
                    columnSpacing: d.headerGap
                    rowSpacing: d.headerGap

                    ColumnLayout {
                        spacing: d.brandGap

                        Image {
                            Layout.alignment: Qt.AlignLeft
                            source: BasecampIcons.logo
                            sourceSize.height: 34
                            fillMode: Image.PreserveAspectFit
                        }

                        RowLayout {
                            Layout.alignment: Qt.AlignLeft
                            spacing: Theme.spacing.small

                            ColumnLayout {
                                spacing: 0

                                LogosText {
                                    text: qsTr("Logos")
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: Theme.palette.textTertiary
                                }

                                LogosText {
                                    objectName: "welcomePage.wordmark"
                                    text: qsTr("Basecamp.")
                                    font.pixelSize: Theme.typography.panelTitleText
                                    font.weight: Theme.typography.weightBold
                                    color: Theme.palette.text
                                }
                            }

                            LogosBadge {
                                id: alphaBadge
                                Layout.alignment: Qt.AlignBottom
                                Layout.bottomMargin: Theme.spacing.tiny
                                text: qsTr("ALPHA")
                                color: Theme.palette.accentOrange
                                backgroundColor: "transparent"
                                radius: Theme.spacing.radiusPill
                                horizontalPadding: 6
                                verticalPadding: 2

                                Binding {
                                    target: alphaBadge.labelItem
                                    property: "font.pixelSize"
                                    value: Theme.typography.badgeText
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignRight
                        spacing: d.cardGap

                        ActionCard {
                            objectName: "welcomePage.discoverApplications"
                            scaleFactor: d.cardScale
                            verb: qsTr("Discover")
                            noun: qsTr("Applications")
                            iconSource: BasecampIcons.dashboard
                            onClicked: root.discoverApplicationsClicked()
                        }

                        ActionCard {
                            objectName: "welcomePage.managePackages"
                            scaleFactor: d.cardScale
                            verb: qsTr("Manage")
                            noun: qsTr("Packages")
                            iconSource: BasecampIcons.modules
                            onClicked: root.managePackagesClicked()
                        }
                    }
                }

                Divider {}

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: d.greetingToSearch

                    LogosText {
                        objectName: "welcomePage.greeting"
                        Layout.fillWidth: true
                        font.pixelSize: d.greetingSize
                        color: Theme.palette.textSecondary
                        elide: Text.ElideRight
                        text: d.hasApps ? qsTr("Welcome Back,")
                                        : qsTr("Welcome to Basecamp!")
                    }

                    RowLayout {
                        objectName: "welcomePage.searchRow"
                        Layout.fillWidth: true
                        spacing: d.searchRowGap

                        LogosSearchBar {
                            id: searchField

                            objectName: "welcomePage.search"
                            focus: true
                            Layout.fillWidth: true
                            Layout.preferredHeight: d.searchFieldHeight
                            Layout.maximumHeight: d.searchFieldHeight
                            placeholderText: qsTr("Search...")
                            shortcutHint: searchShortcut.nativeText
                        }

                        FilterChip {
                            objectName: "welcomePage.filterApps"
                            iconSource: BasecampIcons.dashboard
                            filterValue: "ui_qml"
                            Accessible.name: qsTr("Filter to applications")
                        }

                        FilterChip {
                            objectName: "welcomePage.filterPackages"
                            iconSource: BasecampIcons.modules
                            filterValue: "core"
                            Accessible.name: qsTr("Filter to packages")
                        }
                    }
                }

                Divider {}

                ColumnLayout {
                    objectName: "welcomePage.recentlyClosed"
                    Layout.fillWidth: true
                    spacing: d.sectionGap
                    visible: !d.searching && d.recentApps.length > 0

                    LogosText {
                        Layout.fillWidth: true
                        text: qsTr("Recently Closed")
                        font.pixelSize: Theme.typography.panelTitleText
                        color: Theme.palette.textSecondary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: d.tileGap

                        Repeater {
                            model: d.recentApps

                            delegate: AppGridDelegate {
                                id: recentDelegate

                                required property var modelData

                                objectName: "welcomePage.recent." + recentDelegate.modelData.name
                                width: d.cellWidth
                                height: d.cellHeight
                                tileSize: d.tileSize

                                appData: ({
                                    name: recentDelegate.modelData.name,
                                    displayName: recentDelegate.modelData.displayName
                                                 || recentDelegate.modelData.name,
                                    iconUrl: recentDelegate.modelData.iconPath || "",
                                    supportsFullBleedIcon:
                                        recentDelegate.modelData.supportsFullBleedIcon === true,
                                    isInstalled: true,
                                    installStatus: InstallStatus.Installed,
                                    repositoryUrl: "",
                                    installType: ""
                                })

                                onAppClicked: (name) => root.reopenAppRequested(name)

                                contextMenuEnabled: false
                            }
                        }

                        Item { Layout.fillWidth: true }
                    }
                }

                // Results occupy the same slot as Recently Closed — only one is ever
                // visible. With no filter both groups show; a chip narrows to one.
                ColumnLayout {
                    objectName: "welcomePage.searchResults"
                    Layout.fillWidth: true
                    spacing: d.sectionGap
                    visible: d.searching

                    // No heading: each row's leading icon says which kind it is,
                    // which also buys the vertical room for a second row.
                    ResultRow {
                        objectName: "welcomePage.results.apps"
                        iconSource: BasecampIcons.dashboard
                        proxy: appsProxy
                        active: d.typeFilter === "" || d.typeFilter === "ui_qml"

                        onActivated: (name, repo) => root.appActivated(name, repo)
                        onShowAll: root.showAllResultsRequested("ui_qml", d.query)
                    }

                    ResultRow {
                        objectName: "welcomePage.results.packages"
                        iconSource: BasecampIcons.modules
                        proxy: packagesProxy
                        active: d.typeFilter === "" || d.typeFilter === "core"

                        onActivated: (name, repo, status) =>
                            status === InstallStatus.NotInstalled
                                ? root.packageInstallRequested(name)
                                : root.packageActivated(name)
                        onShowAll: root.showAllResultsRequested("core", d.query)
                    }

                    LogosText {
                        objectName: "welcomePage.noResults"
                        Layout.fillWidth: true
                        visible: d.noMatches && !d.catalogLoading
                        text: qsTr("No results for \u201C%1\u201D").arg(d.query)
                        font.pixelSize: Theme.typography.subtitleText
                        color: Theme.palette.textTertiary
                    }

                    LogosText {
                        objectName: "welcomePage.searchingCatalog"
                        Layout.fillWidth: true
                        visible: d.noMatches && d.catalogLoading
                        text: qsTr("Searching the catalog\u2026")
                        font.pixelSize: Theme.typography.subtitleText
                        color: Theme.palette.textTertiary
                    }
                }
            }
        }
    }

    // One proxy, not one per type: the results are a single combined row, and
    // the chips narrow it rather than choosing between two lists.
    AppsFilterProxy {
        id: appsProxy

        sourceModel: typeof backend !== "undefined" && backend ? backend.appsModel : null
        typeFilter: "ui_qml"
        excludeMainUi: true
        searchText: d.query
    }

    AppsFilterProxy {
        id: packagesProxy

        sourceModel: typeof backend !== "undefined" && backend ? backend.appsModel : null
        typeFilter: "core"
        excludeMainUi: true
        searchText: d.query
    }

    component FilterChip: LogosAbstractButton {
        id: chip

        property url iconSource
        property string filterValue: ""
        readonly property bool active: d.typeFilter === chip.filterValue

        implicitWidth: d.filterButtonSize
        implicitHeight: d.filterButtonSize

        onClicked: d.typeFilter = chip.active ? "" : chip.filterValue

        background: Rectangle {
            radius: width / 2
            color: chip.active     ? Theme.palette.accentOrange
                 : chip.isActive   ? Theme.palette.surfaceInteractiveHover
                                   : Theme.palette.surface
        }

        contentItem: Item {
            LogosIcon {
                anchors.centerIn: parent
                width: d.filterIconSize
                height: d.filterIconSize
                source: chip.iconSource
                color: chip.active ? Theme.palette.backgroundBlack
                                   : Theme.palette.textTertiary
            }
        }
    }

    component Divider: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Theme.palette.borderTertiaryMuted
    }

    component ResultRow: RowLayout {
        id: row

        property url iconSource
        property var proxy: null
        property bool active: true

        signal activated(string moduleName, string repositoryUrl, int installStatus)
        signal showAll()

        readonly property int matches: row.proxy ? row.proxy.visibleCount : 0
        readonly property int slots:
            row.matches > d.resultCapacity ? Math.max(1, d.resultCapacity - 1)
                                           : d.resultCapacity
        readonly property int overflow: Math.max(0, row.matches - row.slots)

        Layout.fillWidth: true
        spacing: d.resultGap
        visible: row.active && row.matches > 0

        LogosIcon {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: (d.resultTileSize - d.rowIconSize) / 2
            width: d.rowIconSize
            height: d.rowIconSize
            source: row.iconSource
            color: Theme.palette.textTertiary
        }

        Repeater {
            model: row.proxy

            delegate: AppGridDelegate {
                // Layouts skip invisible items, so hiding caps the row.
                visible: index < row.slots
                width: d.resultCellWidth
                height: d.resultCellHeight
                tileSize: d.resultTileSize
                appData: model

                onAppClicked:         (name, repo) => row.activated(name, repo,
                                                                     model.installStatus)
                contextMenuEnabled: false
            }
        }

        LogosAbstractButton {
            id: moreChip

            visible: row.overflow > 0
            implicitWidth: d.resultCellWidth
            implicitHeight: d.resultCellHeight
            Accessible.name: qsTr("Show all results")
            onClicked: row.showAll()

            background: Item {
                Rectangle {
                    width: d.resultTileSize
                    height: d.resultTileSize
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    radius: Theme.spacing.radiusXlarge
                    color: moreChip.isActive ? Theme.palette.surfaceInteractiveHover
                                             : Theme.palette.surface

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: Theme.spacing.tiny

                        LogosIcon {
                            Layout.alignment: Qt.AlignHCenter
                            width: d.rowIconSize
                            height: d.rowIconSize
                            source: row.iconSource
                            color: Theme.palette.textTertiary
                        }

                        LogosText {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("+%1").arg(row.overflow)
                            font.pixelSize: Theme.typography.subtitleText
                            color: Theme.palette.textSecondary
                        }
                    }
                }
            }
        }

        Item { Layout.fillWidth: true }
    }

    component ActionCard: LogosAbstractButton {
        id: card

        property string verb: ""
        property string noun: ""
        property url iconSource
        property real scaleFactor: 1.0

        readonly property int chipSize: Math.round(90 * card.scaleFactor)
        readonly property int iconSize: Math.round(50 * card.scaleFactor)
        readonly property int labelSize: Math.round(20 * card.scaleFactor)
        readonly property int contentGap: Math.round(33 * card.scaleFactor)

        Accessible.name: card.verb + " " + card.noun

        implicitWidth: Math.round(300 * card.scaleFactor)
        implicitHeight: Math.round(136 * card.scaleFactor)

        leftPadding: Math.round(28 * card.scaleFactor)
        rightPadding: card.leftPadding
        topPadding: Math.round(23 * card.scaleFactor)
        bottomPadding: card.topPadding

        background: Rectangle {
            radius: Math.round(24 * card.scaleFactor)
            color: card.isActive ? Theme.palette.surfaceInteractiveHover
                                 : Theme.palette.backgroundSecondary
        }

        contentItem: RowLayout {
            spacing: card.contentGap

            Rectangle {
                Layout.preferredWidth: card.chipSize
                Layout.preferredHeight: card.chipSize
                Layout.alignment: Qt.AlignVCenter
                radius: width / 2
                color: Theme.palette.surface

                LogosIcon {
                    anchors.centerIn: parent
                    width: card.iconSize
                    height: card.iconSize
                    source: card.iconSource
                    color: Theme.palette.textTertiary
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                LogosText {
                    Layout.fillWidth: true
                    text: card.verb
                    font.pixelSize: card.labelSize
                    color: Theme.palette.textTertiary
                    elide: Text.ElideRight
                }

                LogosText {
                    Layout.fillWidth: true
                    text: card.noun
                    font.pixelSize: card.labelSize
                    font.weight: Theme.typography.weightBold
                    color: Theme.palette.text
                    elide: Text.ElideRight
                }
            }
        }
    }
}
