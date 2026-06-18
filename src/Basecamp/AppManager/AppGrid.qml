import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Theme

Item {
    id: root

    // ─── Public API ───
    property var modulesSource: null
    property string viewMode: "grid"
    property string emptyText: qsTr("No applications.")
    signal appClicked(string name, string repositoryUrl)
    signal appManageRequested(string name, string repositoryUrl)

    QtObject {
        id: d

        readonly property int gridCellWidth:  180
        readonly property int gridCellHeight: 162
        readonly property int listRowHeight: 64

        readonly property bool isList: root.viewMode === "list"
    }

    implicitHeight: grid.count === 0
                        ? emptyLabel.implicitHeight + Theme.spacing.large * 2
                        : grid.contentHeight

    GridView {
        id: grid
        anchors.fill: parent
        cellWidth:  d.isList ? width             : d.gridCellWidth
        cellHeight: d.isList ? d.listRowHeight   : d.gridCellHeight
        model: root.modulesSource
        interactive: false
        clip: true

        delegate: d.isList ? listDelegate : gridDelegate
    }

    LogosText {
        id: emptyLabel
        anchors.centerIn: parent
        visible: grid.count === 0
        text: root.emptyText
        color: Theme.palette.textPlaceholder
        font.pixelSize: Theme.typography.primaryText
    }

    Component {
        id: gridDelegate
        AppGridDelegate {
            width:  GridView.view.cellWidth
            height: GridView.view.cellHeight
            appData: model
            onAppClicked: function(name, repoUrl) {
                root.appClicked(name, repoUrl)
            }
            onManageRequested: function(name, repoUrl) {
                root.appManageRequested(name, repoUrl)
            }
        }
    }

    Component {
        id: listDelegate
        AppListDelegate {
            width:  GridView.view.cellWidth
            height: GridView.view.cellHeight
            appData: model
            onAppClicked: function(name, repoUrl) {
                root.appClicked(name, repoUrl)
            }
            onManageRequested: function(name, repoUrl) {
                root.appManageRequested(name, repoUrl)
            }
        }
    }
}
