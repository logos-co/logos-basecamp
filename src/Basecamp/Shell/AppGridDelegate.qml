import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme
import Basecamp.Backend 1.0
import Basecamp.Installer

ItemDelegate {
    id: root

    // ─── Public API ───
    property var appData: ({})
    property int tileSize: 80
    property int labelSize: Theme.typography.subtitleText
    signal appClicked(string name, string repositoryUrl)

    QtObject {
        id: d

        readonly property bool isInstalled: !!root.appData && root.appData.isInstalled !== false
        readonly property bool hasUpdate:   !!root.appData && root.appData.hasUpdate === true
        readonly property int installStatus:
            root.appData && root.appData.installStatus !== undefined
                ? root.appData.installStatus
                : InstallStatus.NotInstalled
        // What the badge offers for the app's status.
        readonly property int statusAction:
            d.installStatus === InstallStatus.UpgradeAvailable   ? Types.RowAction.Upgrade
            : d.installStatus === InstallStatus.DowngradeAvailable ? Types.RowAction.Downgrade
            : d.installStatus === InstallStatus.DifferentHash      ? Types.RowAction.Reinstall
            : Types.RowAction.Install

        readonly property string nameText:      root.appData ? (root.appData.name || "") : ""
        readonly property string displayName:   root.appData ? (root.appData.displayName || root.appData.name || "") : ""
        readonly property string iconUrl:      root.appData ? (root.appData.iconUrl || "") : ""
        // 0.4.0+ guarantees a validated 256x256 icon, so it can fill the
        // tile. Older packages ship a small glyph that must stay inset.
        readonly property bool fullBleedIcon:
            !!root.appData && root.appData.supportsFullBleedIcon === true
        readonly property string repositoryUrl: root.appData ? (root.appData.repositoryUrl || "") : ""
        readonly property bool isAvailable:
            !root.appData
            || root.appData.hasInstallableVersion !== false
            || d.isInstalled

        readonly property real tileOpacity:
            !d.isAvailable            ? 0.35
            : (d.isInstalled || root.hovered) ? 1.0 : 0.55

        readonly property int tileSize: root.tileSize

    }

    background: Item {}
    padding: 0
    hoverEnabled: true
    onClicked: if (d.isAvailable) root.appClicked(d.nameText, d.repositoryUrl)

    contentItem: Item {
        ColumnLayout {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.spacing.small
            anchors.rightMargin: Theme.spacing.small
            spacing: Theme.spacing.medium

            Item {
                id: tile
                Layout.preferredWidth: d.tileSize
                Layout.preferredHeight: d.tileSize
                Layout.alignment: Qt.AlignHCenter

                LogosTile {
                    objectName: "appGridDelegate.tile"
                    anchors.fill: parent
                    label: d.nameText
                    source: d.iconUrl
                    // No colour of its own, as the sidebar shows it.
                    fallbackColor: Theme.palette.surfaceRaised
                    tileSize: d.tileSize
                    radius: Theme.spacing.radiusXlarge
                    dimOpacity: d.tileOpacity
                    insetArtwork: !d.fullBleedIcon
                    interactive: false
                }

                LogosBadge {
                    id: stateBadge
                    objectName: "appGridStateBadge"
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottomMargin: Theme.spacing.tiny
                    visible: !d.isAvailable
                             || (d.installStatus !== InstallStatus.Installed
                                 && (d.isInstalled || root.hovered))
                    text: !d.isAvailable ? Stages.actionWord(Types.RowAction.NotAvailable)
                                         : Stages.actionWord(d.statusAction)
                    color: !d.isAvailable                                      ? Theme.palette.textTertiary
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
                id: label

                objectName: "appGridDelegate.label"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                maximumLineCount: 2
                text: d.displayName
                font.pixelSize: root.labelSize
                color: d.isInstalled ? Theme.palette.text : Theme.palette.textSubtle
                elide: Text.ElideRight
                LogosToolTip {
                    text: d.displayName
                    placement: LogosToolTip.Bottom
                    visible: root.hovered && label.truncated
                }
            }
        }
    }
}
