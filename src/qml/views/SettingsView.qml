import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

Rectangle {
    id: root

    property var    repositories:        []
    property bool   repositoriesLoading: false

    signal repositoryRefreshRequested()
    signal repositoryAddRequested(string url)
    signal repositoryRemoveRequested(string url)
    signal repositoryEnabledRequested(string url, bool enabled)
    signal repositoriesBecameVisible()

    function reportRepositoryResult(operation, url, success, error) {
        repositoriesView.reportOperationResult(operation, url, success, error)
    }

    function showSection(idx) {
        if (idx >= 0 && idx < d.sections.length)
            d.selectedIndex = idx
    }

    QtObject {
        id: d

        // Sub-views in the right pane. Order maps 1:1 to the StackLayout below.
        readonly property var sections: [
            { label: qsTr("Dashboard") },
            { label: qsTr("Modules") },
            { label: qsTr("Package Repositories") }
        ]

        property int selectedIndex: 0
    }

    color: Theme.palette.background

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.xxlarge
        spacing: Theme.spacing.xlarge

        // ─── Header ───
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.tiny

            LogosText {
                text: qsTr("Settings")
                font.pixelSize: Theme.typography.pageTitleText
                font.weight: Theme.typography.weightBold
                color: Theme.palette.text
            }

            LogosText {
                text: qsTr("Manage modules, apps and dashboards.")
                font.pixelSize: Theme.typography.primaryText
                color: Theme.palette.textSecondary
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing.xlarge

            // ─── Sections sidebar (mirrors AppManagerView's categories pane) ───
            ColumnLayout {
                Layout.preferredWidth: 200
                Layout.minimumWidth: 160
                Layout.maximumWidth: 200
                Layout.fillHeight: true
                spacing: Theme.spacing.tiny

                LogosText {
                    Layout.topMargin: Theme.spacing.tiny
                    Layout.bottomMargin: Theme.spacing.tiny
                    text: qsTr("Sections")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightRegular
                    color: Theme.palette.text
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: contentHeight
                    interactive: false
                    spacing: Theme.spacing.tiny
                    model: d.sections
                    currentIndex: d.selectedIndex

                    delegate: LogosItemDelegate {
                        width: ListView.view.width
                        text: modelData.label
                        highlighted: ListView.isCurrentItem
                        radius: Theme.spacing.radiusLarge
                        highlightColor: Theme.palette.backgroundButton
                        hoverColor: "transparent"
                        textColor: (highlighted || hovered)
                                       ? Theme.palette.text
                                       : Theme.palette.textTertiary
                        onClicked: d.selectedIndex = index
                    }
                }

                Item { Layout.fillWidth: true; Layout.fillHeight: true }
            }

            // ─── Right pane ───
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.palette.surfaceRaised
                radius: Theme.spacing.radiusXlarge
                clip: true

                StackLayout {
                    anchors.fill: parent
                    currentIndex: d.selectedIndex

                    // 0 — Dashboard.
                    DashboardView {}

                    // 1 — Modules.
                    ModulesView {
                        onVisibleChanged: if (visible) Qt.callLater(() => {
                            backend.refreshUiModules()
                            backend.refreshCoreModules()
                        })
                    }

                    // 2 — Package Repositories.
                    RepositoriesView {
                        id: repositoriesView

                        repositories: root.repositories
                        loading:      root.repositoriesLoading

                        onRefreshRequested:    root.repositoryRefreshRequested()
                        onAddRequested:        url => root.repositoryAddRequested(url)
                        onRemoveRequested:     url => root.repositoryRemoveRequested(url)
                        onSetEnabledRequested: (url, enabled) =>
                                                   root.repositoryEnabledRequested(url, enabled)
                        onVisibleChanged:      if (visible)
                                                   root.repositoriesBecameVisible()
                    }
                }
            }
        }
    }
}
