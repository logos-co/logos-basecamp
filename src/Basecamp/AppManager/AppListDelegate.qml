import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Theme
import Basecamp.Backend 1.0

ItemDelegate {
    id: root

    // ─── Public API ───
    property var appData: ({})
    signal appClicked(string name, string repositoryUrl)
    signal manageRequested(string name, string repositoryUrl)

    QtObject {
        id: d

        readonly property bool isInstalled: !!root.appData && root.appData.isInstalled !== false
        readonly property bool hasUpdate:   !!root.appData && root.appData.hasUpdate === true
        readonly property int installStatus:
            root.appData && root.appData.installStatus !== undefined
                ? root.appData.installStatus
                : InstallStatus.NotInstalled
        readonly property int installStage:
            root.appData && root.appData.installStage !== undefined
                ? root.appData.installStage
                : InstallStage.None
        readonly property bool isInstalling:
            d.installStage === InstallStage.Downloading
            || d.installStage === InstallStage.Queued
            || d.installStage === InstallStage.Installing

        readonly property string nameText:      root.appData ? (root.appData.name || "") : ""
        readonly property string displayName:   root.appData ? (root.appData.displayName || root.appData.name || "") : ""
        readonly property string iconUrl:      root.appData ? (root.appData.iconUrl || "") : ""
        readonly property string description:   root.appData ? (root.appData.description || "") : ""
        readonly property string repositoryUrl: root.appData ? (root.appData.repositoryUrl || "") : ""
        readonly property string packageColor: root.appData ? (root.appData.color || "") : ""
        readonly property real tileOpacity:
            (d.isInstalled || root.hovered) ? 1.0 : 0.55

        readonly property int tileSize: 40
    }

    padding: 0
    hoverEnabled: true

    onClicked: root.appClicked(d.nameText, d.repositoryUrl)

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: root.manageRequested(d.nameText, d.repositoryUrl)
    }

    background: Rectangle {
        anchors.fill: parent
        anchors.margins: Theme.spacing.tiny
        radius: Theme.spacing.radiusLarge
        color: root.hovered ? Theme.palette.surfaceRaised : "transparent"
    }

    contentItem: RowLayout {
        anchors.leftMargin: Theme.spacing.medium
        anchors.rightMargin: Theme.spacing.medium
        spacing: Theme.spacing.medium

        AppTile {
            Layout.preferredWidth: d.tileSize
            Layout.preferredHeight: d.tileSize
            Layout.alignment: Qt.AlignVCenter
            appName: d.nameText
            packageColor: d.packageColor
            iconSource: d.iconUrl
            tileSize: d.tileSize
            iconSize: 24
            dimOpacity: d.tileOpacity
        }

        // Name + (optional) description.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 0

            LogosText {
                Layout.fillWidth: true
                text: d.displayName
                font.pixelSize: Theme.typography.primaryText
                font.weight: Theme.typography.weightMedium
                color: d.isInstalled ? Theme.palette.text : Theme.palette.textSubtle
                elide: Text.ElideRight
            }

            LogosText {
                Layout.fillWidth: true
                text: d.description
                font.pixelSize: Theme.typography.secondaryText
                color: Theme.palette.textTertiary
                elide: Text.ElideRight
                visible: text.length > 0
            }
        }

        LogosBadge {
            Layout.alignment: Qt.AlignVCenter
            // Installed-but-stale states stay visible; NotInstalled only on hover.
            visible: d.isInstalling
                     || d.installStage === InstallStage.Failed
                     || (d.installStatus !== InstallStatus.Installed
                         && (d.isInstalled || root.hovered))
            text: d.isInstalling                                      ? qsTr("Installing…")
                : d.installStage === InstallStage.Failed              ? qsTr("Failed")
                : d.installStatus === InstallStatus.UpgradeAvailable      ? qsTr("Update")
                : d.installStatus === InstallStatus.DowngradeAvailable    ? qsTr("Downgrade")
                : d.installStatus === InstallStatus.DifferentHash         ? qsTr("Reinstall")
                                                                      : qsTr("Install")
            color: d.isInstalling                                      ? Theme.palette.warning
                 : d.installStage === InstallStage.Failed              ? Theme.palette.error
                 : d.installStatus === InstallStatus.UpgradeAvailable      ? Theme.palette.info
                 : d.installStatus === InstallStatus.DowngradeAvailable    ? Theme.palette.info
                 : d.installStatus === InstallStatus.DifferentHash         ? Theme.palette.info
                                                                       : Theme.palette.accentOrange
            backgroundColor: Theme.palette.surfaceRaised
        }
    }
}
