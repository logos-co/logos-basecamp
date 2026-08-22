import QtQuick
import QtQuick.Controls
import Logos.Controls
import Logos.Theme
import Basecamp.Backend 1.0

LogosMenu {
    id: root

    // Plain object snapshot of the row: name, displayName, repositoryUrl,
    // isInstalled, installStage, installStatus, installType. Set via
    // openFor() — never bound to a delegate's `model`, which would dangle
    // once the row scrolls out from under an open menu.
    property var appData: ({})

    signal openRequested(string name, string repositoryUrl)
    signal detailsRequested(string name, string repositoryUrl)
    signal installRequested(string name, string repositoryUrl)
    signal uninstallRequested(string name, string repositoryUrl)

    function openFor(appData_) {
        root.appData = appData_ || ({});
        root.popup();
    }


    QtObject {
        id: d

        readonly property string rowName:
            (root.appData && root.appData.name) ? root.appData.name : ""
        readonly property string rowRepositoryUrl:
            (root.appData && root.appData.repositoryUrl) ? root.appData.repositoryUrl : ""
        readonly property bool rowInstalled:
            !!root.appData && root.appData.isInstalled === true
        readonly property int rowStage:
            (root.appData && root.appData.installStage !== undefined)
                ? root.appData.installStage
                : InstallStage.None
        // Install/upgrade in flight — the module's pending slot is global,
        // so a racing uninstall would just be rejected. Disable rather than
        // hide so the item doesn't jump around mid-install.
        readonly property bool rowBusy:
            d.rowStage === InstallStage.Queued
            || d.rowStage === InstallStage.Downloading
            || d.rowStage === InstallStage.Installing
        readonly property bool rowEmbedded:
            !!root.appData && root.appData.installType === "embedded"
        readonly property bool rowProtected:
            d.rowName === "main_ui"
        readonly property bool canUninstall:
            d.rowInstalled && !d.rowEmbedded && !d.rowProtected
    }

    LogosMenuItem {
        objectName: "appContextMenu.open"
        text: qsTr("Open")
        visible: d.rowInstalled
        height: visible ? implicitHeight : 0
        onTriggered: root.openRequested(d.rowName, d.rowRepositoryUrl)
    }

    LogosMenuItem {
        objectName: "appContextMenu.details"
        text: qsTr("Details")
        visible: d.rowInstalled
        height: visible ? implicitHeight : 0
        onTriggered: root.detailsRequested(d.rowName, d.rowRepositoryUrl)
    }

    LogosMenuItem {
        objectName: "appContextMenu.install"
        text: qsTr("Install…")
        visible: !d.rowInstalled
        height: visible ? implicitHeight : 0
        onTriggered: root.installRequested(d.rowName, d.rowRepositoryUrl)
    }

    LogosMenuSeparator {
        visible: d.canUninstall
        height: visible ? implicitHeight : 0
    }

    LogosMenuItem {
        id: uninstallItem
        objectName: "appContextMenu.uninstall"
        text: qsTr("Uninstall")
        visible: d.canUninstall
        height: visible ? implicitHeight : 0
        enabled: !d.rowBusy
        onTriggered: root.uninstallRequested(d.rowName, d.rowRepositoryUrl)

        contentItem: LogosText {
            leftPadding: Theme.spacing.medium
            rightPadding: Theme.spacing.medium
            verticalAlignment: Text.AlignVCenter
            text: uninstallItem.text
            color: uninstallItem.enabled ? Theme.palette.error
                                         : Theme.palette.textMuted
        }
    }
}
