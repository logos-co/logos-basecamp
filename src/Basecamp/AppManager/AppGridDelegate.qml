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
    signal detailsRequested(string name, string repositoryUrl)
    signal installRequested(string name, string repositoryUrl)
    signal uninstallRequested(string name, string repositoryUrl)

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
        // 0.4.0+ guarantees a validated 256x256 icon, so it can fill the
        // tile. Older packages ship a small glyph that must stay inset.
        readonly property bool fullBleedIcon:
            !!root.appData && root.appData.supportsFullBleedIcon === true
        readonly property string repositoryUrl: root.appData ? (root.appData.repositoryUrl || "") : ""
        readonly property string installType:   root.appData ? (root.appData.installType || "") : ""
        readonly property real tileOpacity:
            (d.isInstalled || root.hovered) ? 1.0 : 0.55

        readonly property int tileSize: 80

        // Plain-object copy of the row for the context menu. `model` itself is
        // owned by the delegate and would dangle if the row recycled while the
        // menu is up, so snapshot the fields the menu reads.
        function snapshot() {
            return {
                name:          d.nameText,
                displayName:   d.displayName,
                repositoryUrl: d.repositoryUrl,
                isInstalled:   d.isInstalled,
                installStage:  d.installStage,
                installStatus: d.installStatus,
                installType:   d.installType,
            };
        }
    }

    background: Item {}
    padding: 0
    hoverEnabled: true

    onClicked: root.appClicked(d.nameText, d.repositoryUrl)

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: contextMenu.openFor(d.snapshot())
    }

    AppContextMenu {
        id: contextMenu
        onOpenRequested:      (name, repo) => root.appClicked(name, repo)
        onDetailsRequested:   (name, repo) => root.detailsRequested(name, repo)
        onInstallRequested:   (name, repo) => root.installRequested(name, repo)
        onUninstallRequested: (name, repo) => root.uninstallRequested(name, repo)
    }

    contentItem: Item {
        ColumnLayout {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.spacing.medium

            Item {
                id: tile
                Layout.preferredWidth: d.tileSize
                Layout.preferredHeight: d.tileSize
                Layout.alignment: Qt.AlignHCenter

                LogosTile {
                    anchors.fill: parent
                    label: d.nameText
                    source: d.iconUrl
                    fallbackColor: d.isInstalled
                                   ? Theme.palette.surfaceRaised
                                   : AppColors.colorForApp(d.nameText)
                    tileSize: d.tileSize
                    radius: Theme.spacing.radiusXlarge
                    dimOpacity: d.tileOpacity
                    insetArtwork: !d.fullBleedIcon
                    interactive: false
                }

                LogosBadge {
                    id: stateBadge
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottomMargin: Theme.spacing.tiny
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
                    radius: Theme.spacing.radiusXlarge
                    Component.onCompleted: if (labelItem) labelItem.font.pixelSize = Theme.typography.badgeText
                }
            }

            LogosText {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: d.tileSize
                horizontalAlignment: Text.AlignHCenter
                text: d.displayName
                font.pixelSize: Theme.typography.subtitleText
                color: d.isInstalled ? Theme.palette.text : Theme.palette.textSubtle
                elide: Text.ElideRight
            }
        }
    }
}
