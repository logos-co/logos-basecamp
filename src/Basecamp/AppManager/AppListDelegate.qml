import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Icons
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
        // The SESSION's stage, not this package's. A tile represents the
        // whole install: its own package downloads last, so binding to the
        // package's stage left the tile idle while its dependencies were
        // being fetched. planInstallStage is derived across the plan.
        readonly property int installStage:
            root.appData && root.appData.planInstallStage !== undefined
                ? root.appData.planInstallStage
                : InstallStage.None
        readonly property bool isInstalling:
            d.installStage === InstallStage.Downloading
            || d.installStage === InstallStage.Downloaded
            || d.installStage === InstallStage.Queued
            || d.installStage === InstallStage.Installing

        // Live download bytes. Only meaningful during Downloading; the
        // stages either side of it have no byte count, so the badge falls
        // back to "Installing…" there rather than freezing on a number.
        readonly property real dlReceived:
            root.appData ? (root.appData.planDownloadReceived || 0) : 0
        readonly property real dlTotal:
            root.appData ? (root.appData.planDownloadTotal || 0) : 0
        readonly property bool downloadDone: d.dlTotal > 0 && d.dlReceived >= d.dlTotal
        // A determinate bar claims a FRACTION, so it needs a real sample.
        // dlTotal is seeded from the catalog when the plan is registered —
        // before any transfer — so gating on it alone parked the bar at 0%.
        readonly property bool hasProgress:
            d.installStage === InstallStage.Downloading && d.dlTotal > 0
            && d.dlReceived > 0 && !d.downloadDone
        // Nothing measurable yet: no bytes, or no size from transport or
        // catalog. Sweep rather than sit still.
        readonly property bool indeterminateProgress:
            d.installStage === InstallStage.Downloading && !d.downloadDone
            && (d.dlReceived <= 0 || d.dlTotal <= 0)

        readonly property string nameText:      root.appData ? (root.appData.name || "") : ""
        readonly property string displayName:   root.appData ? (root.appData.displayName || root.appData.name || "") : ""
        readonly property string iconUrl:      root.appData ? (root.appData.iconUrl || "") : ""
        readonly property string description:   root.appData ? (root.appData.description || "") : ""
        // 0.4.0+ guarantees a validated 256x256 icon, so it can fill the
        // tile. Older packages ship a small glyph that must stay inset.
        readonly property bool fullBleedIcon:
            !!root.appData && root.appData.supportsFullBleedIcon === true
        readonly property string repositoryUrl: root.appData ? (root.appData.repositoryUrl || "") : ""
        readonly property string installType:   root.appData ? (root.appData.installType || "") : ""
        readonly property real tileOpacity:
            (d.isInstalled || root.hovered) ? 1.0 : 0.55

        readonly property int tileSize: 40

        // Mirrors AppContextMenu.canUninstall so the trash icon and the menu
        // agree on when Uninstall is offered. Kept here rather than in the
        // menu snapshot because the icon is shown before any right-click.
        readonly property bool isEmbedded:  d.installType === "embedded"
        readonly property bool isProtected: d.nameText === "main_ui"
        readonly property bool canUninstall:
            d.isInstalled && !d.isEmbedded && !d.isProtected && !d.isInstalling

        // See AppGridDelegate — snapshot rather than hand `model` to the menu.
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

        LogosTile {
            Layout.preferredWidth: d.tileSize
            Layout.preferredHeight: d.tileSize
            Layout.alignment: Qt.AlignVCenter
            label: d.nameText
            source: d.iconUrl
            fallbackColor: d.isInstalled
                           ? Theme.palette.surfaceRaised
                           : AppColors.colorForApp(d.nameText)
            tileSize: d.tileSize
            dimOpacity: d.tileOpacity
            insetArtwork: !d.fullBleedIcon
            interactive: false
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
            id: stateBadge
            TextMetrics {
                id: widestInstallLabel
                font: stateBadge.labelItem.font
                text: "999.9 / 999.9 MB"
            }
            property int pillWidth: (d.isInstalling)
                ? Math.ceil(widestInstallLabel.width) + leftPadding + rightPadding
                : implicitWidth
            Behavior on pillWidth {
                NumberAnimation { duration: 120; easing.type: Easing.OutQuad }
            }
            Layout.preferredWidth: pillWidth
            Layout.alignment: Qt.AlignVCenter
            // Installed-but-stale states stay visible; NotInstalled only on hover.
            visible: d.isInstalling
                     || d.installStage === InstallStage.Failed
                     || (d.installStatus !== InstallStatus.Installed
                         && (d.isInstalled || root.hovered))
            // While bytes are moving the badge counts them; the rest of the
            // install (verify, install) has no byte count, so it reverts to
            // the plain label instead of parking at 100%.
            text: d.hasProgress ? DownloadFormat.label(d.dlReceived, d.dlTotal)
                : d.isInstalling                                      ? qsTr("Installing…")
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

            LogosProgressBar {
                parent: stateBadge.backgroundItem
                visible: d.hasProgress || d.indeterminateProgress
                anchors.left: parent ? parent.left : undefined
                anchors.right: parent ? parent.right : undefined
                anchors.bottom: parent ? parent.bottom : undefined
                anchors.margins: stateBadge.borderWidth + 1
                height: 3

                from: 0
                to: d.dlTotal > 0 ? d.dlTotal : 1
                value: d.dlReceived
                indeterminate: d.indeterminateProgress

                trackColor: "transparent"
                fillColor: Theme.colors.getColor(Theme.palette.warning, 0.95)

                Behavior on value {
                    NumberAnimation { duration: 180; easing.type: Easing.OutQuad }
                }
            }
        }

        // Per-row Uninstall — trash icon in the trailing cell.
        LogosIconButton {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 32
            Layout.preferredHeight: 32
            visible: d.canUninstall
            size: 32
            iconSize: 18
            iconSource: LogosIcons.trash
            background: Item {}
            objectName: "appListDelegate.uninstall"
            onClicked: root.uninstallRequested(d.nameText, d.repositoryUrl)
            LogosToolTip {
                text: qsTr("Uninstall")
                placement: LogosToolTip.Top
                visible: parent.hovered
            }
        }
    }
}
