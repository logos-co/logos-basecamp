import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Icons
import Logos.Theme
import Basecamp.Backend 1.0

Dialog {
    id: root

    property var metadata: ({})
    property var requiredPackagesModel: null
    property int installStage: InstallStage.None
    // Reason for a failed install, shown in the footer. Set by OverlayDialogs'
    // onCatalogInstallFailed; cleared on each (re)open.
    property string installError: ""

    signal installRequested(string name, string repositoryUrl, var versionPins)
    signal launchRequested(string name)
    signal versionChangeRequested(string name, string repositoryUrl, var versionPins)

    function openWith(metadata_) {
        root.metadata = metadata_ || ({})
        d.pickedVersions = ({})
        root.installStage = root.metadata.installStage || InstallStage.None
        root.installError = ""   // clear any stale error from a prior open
        open()
    }

    function markInstallComplete() {
        root.metadata = Object.assign({}, root.metadata, {
            isInstalled: true,
            installStatus: InstallStatus.Installed,
            installedVersion: d.targetVersion,
        })
    }

    QtObject {
        id: d

        readonly property bool installing:
            root.installStage === InstallStage.Downloading
            || root.installStage === InstallStage.Queued
            || root.installStage === InstallStage.Installing
        readonly property string stageLabel: {
            switch (root.installStage) {
            case InstallStage.Downloading: return qsTr("Downloading…")
            case InstallStage.Queued:      return qsTr("Queued")
            case InstallStage.Installing:  return qsTr("Installing…")
            case InstallStage.Installed:   return qsTr("Installed")
            case InstallStage.Failed:      return qsTr("Failed")
            }
            return ""
        }

        property var pickedVersions: ({})

        // ── Target app derived fields ──
        readonly property string targetName:        root.metadata.name || ""
        readonly property string targetRepoUrl:     root.metadata.repositoryUrl || ""
        readonly property string targetVersion:     root.metadata.selectedVersion || ""
        readonly property string targetDisplayName: root.metadata.displayName || root.metadata.name || ""
        readonly property string targetIcon: {
            const raw = root.metadata.icon || ""
            if (raw.length === 0)       return ""
            if (raw.indexOf("://") < 0) return ""
            return raw
        }
        readonly property bool   hasIcon: d.targetIcon.length > 0
        readonly property string packageColor: root.metadata.color || ""
        readonly property color  tileColor:
            d.packageColor.length > 0 ? d.packageColor
                                      : AppColors.colorForApp(d.targetName)

        readonly property int tileSize: 64
        readonly property int monogramSize: Math.round(d.tileSize * 0.375)

        readonly property bool   installed:        root.metadata.isInstalled === true
        readonly property string installedVersion: root.metadata.installedVersion || ""
        readonly property string latestVersion:    root.metadata.latestVersion || ""
        readonly property bool   hasUpdate:
            d.installed && d.latestVersion.length > 0
            && d.installedVersion !== d.latestVersion
        readonly property int installStatus:
            root.metadata.installStatus !== undefined
                ? root.metadata.installStatus
                : InstallStatus.NotInstalled

        readonly property string actionMode: {
            if (d.installing) return "installing"
            switch (d.installStatus) {
            case InstallStatus.UpgradeAvailable:   return "update"
            case InstallStatus.DowngradeAvailable: return "downgrade"
            case InstallStatus.DifferentHash:      return "reinstall"
            case InstallStatus.Installed:          return "launch"
            }
            return "install"
        }
        readonly property string actionText: {
            switch (d.actionMode) {
            case "installing": return d.stageLabel
            case "install":    return qsTr("Install")
            case "update":     return qsTr("Update")
            case "downgrade":  return qsTr("Downgrade")
            case "reinstall":  return qsTr("Reinstall")
            case "launch":     return qsTr("Launch")
            }
            return ""
        }

        readonly property bool actionEnabled:
            d.targetName.length > 0
            && !d.installing
            && (d.actionMode === "launch" || !d.hasResolutionErrors)

        readonly property int totalDeps:
            root.requiredPackagesModel ? root.requiredPackagesModel.visibleCount : 0
        readonly property int installedDepsCount:
            root.requiredPackagesModel ? root.requiredPackagesModel.installedCount : 0
        readonly property bool hasResolutionErrors:
            root.requiredPackagesModel ? root.requiredPackagesModel.hasResolutionErrors : false
        readonly property string counterText:
            d.installedDepsCount + " / " + d.totalDeps

        readonly property var selectedVersionRow: {
            const list = root.metadata.versions || []
            // Version lives at manifest.version, not a top-level `version`;
            // matching the latter always fell through to list[0] (newest).
            for (var i = 0; i < list.length; ++i) {
                const v = (list[i] && list[i].manifest) ? list[i].manifest.version : undefined
                if (v === d.targetVersion) return list[i]
            }
            return list.length > 0 ? list[0] : ({})
        }

        readonly property string sizeText:
            d.formatBytes(root.requiredPackagesModel
                              && root.requiredPackagesModel.totalDownloadBytes > 0
                          ? root.requiredPackagesModel.totalDownloadBytes
                          : d.selectedVersionRow.size)
        readonly property string releasedText: d.formatDate(d.selectedVersionRow.releasedAt)

        function buildVersionPins() {
            const pins = Object.assign({}, d.pickedVersions)
            if (d.targetVersion.length > 0 && !(d.targetName in pins)) {
                pins[d.targetName] = d.targetVersion
            }
            return pins
        }

        function formatBytes(n) {
            if (!n || n <= 0) return ""
            const units = ["B", "KB", "MB", "GB"]
            var i = 0
            var v = n
            while (v >= 1024 && i < units.length - 1) { v /= 1024; ++i }
            return (Math.round(v * 10) / 10) + " " + units[i]
        }

        function formatDate(iso) {
            if (!iso) return ""
            const dt = new Date(iso)
            if (isNaN(dt.getTime())) return ""
            return dt.toLocaleDateString(Qt.locale(), Locale.ShortFormat)
        }
    }

    modal: true
    anchors.centerIn: parent
    width: 560
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.palette.backgroundSecondary
        border.color: Theme.palette.borderSubtle
        border.width: 1
        radius: Theme.spacing.radiusLarge
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing.large

        // ─── Header ───
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacing.large
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large

            LogosText {
                Layout.fillWidth: true
                text: qsTr("Add Application")
                font.pixelSize: Theme.typography.pageTitleText
                font.weight: Theme.typography.weightBold
                color: Theme.palette.text
            }

            LogosIconButton {
                iconSource: LogosIcons.close
                size: 28
                iconSize: 14
                background: Rectangle {
                    radius: width / 2
                    color: Theme.palette.surfaceRaised
                }
                onClicked: root.close()
            }
        }

        // ─── App showcase ───
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.preferredHeight: 200
            color: Theme.palette.background
            radius: Theme.spacing.radiusMedium

            ColumnLayout {
                anchors.centerIn: parent
                spacing: Theme.spacing.small

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: d.tileSize
                    Layout.preferredHeight: d.tileSize
                    color: d.tileColor
                    radius: Theme.spacing.radiusMedium

                    LogosIcon {
                        anchors.centerIn: parent
                        source: d.targetIcon
                        color: Theme.palette.text
                        brightness: 1.0
                        width: 32
                        height: 32
                        visible: d.hasIcon
                    }
                    LogosText {
                        anchors.centerIn: parent
                        visible: !d.hasIcon
                        text: (d.targetDisplayName || "?").substring(0, 2).toUpperCase()
                        font.pixelSize: d.monogramSize
                        font.weight: Theme.typography.weightBold
                        color: Theme.palette.text
                    }
                }

                LogosText {
                    Layout.alignment: Qt.AlignHCenter
                    text: d.targetDisplayName
                    font.pixelSize: Theme.typography.primaryText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }
            }
        }

        // ─── Description ───
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            spacing: Theme.spacing.tiny

            LogosText {
                text: qsTr("Description")
                font.pixelSize: Theme.typography.secondaryText
                color: Theme.palette.textTertiary
            }
            LogosText {
                Layout.fillWidth: true
                text: root.metadata.description || ""
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.typography.primaryText
                color: Theme.palette.text
            }
        }

        // ─── Version row: picker | date | size | Install | overflow ───
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.preferredHeight: 52
            color: Theme.palette.background
            radius: Theme.spacing.radiusMedium

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacing.medium
                anchors.rightMargin: Theme.spacing.small
                spacing: Theme.spacing.large

                LogosText {
                    Layout.alignment: Qt.AlignVCenter
                    text: d.targetVersion ? ("Version " + d.targetVersion) : ""
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.secondaryText
                    font.weight: Theme.typography.weightMedium
                }
                LogosText {
                    Layout.alignment: Qt.AlignVCenter
                    text: d.releasedText
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
                LogosText {
                    Layout.alignment: Qt.AlignVCenter
                    text: d.sizeText
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }

                Item { Layout.fillWidth: true }

                LogosButton {
                    id: actionButton
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: 110
                    Layout.preferredHeight: 40
                    enabled: d.actionEnabled
                    text: d.actionText
                    onClicked: {
                        if (d.actionMode === "launch") {
                            root.launchRequested(d.targetName)
                            root.close()
                            return
                        }
                        root.installRequested(
                            d.targetName, d.targetRepoUrl, d.buildVersionPins())
                    }
                    background: Rectangle {
                        radius: Theme.spacing.radiusXlarge
                        color: !actionButton.enabled
                                   ? Theme.palette.backgroundMuted
                                   : (actionButton.isActive
                                          ? Theme.palette.accentOrangeMid
                                          : Theme.palette.accentOrange)
                    }
                }
            }
        }

        // ─── Required Packages section ───
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.topMargin: Theme.spacing.medium

            LogosText {
                Layout.fillWidth: true
                text: qsTr("Required Packages")
                font.pixelSize: Theme.typography.panelTitleText
                font.weight: Theme.typography.weightMedium
                color: Theme.palette.text
            }
            LogosText {
                text: d.counterText
                font.pixelSize: Theme.typography.primaryText
                color: Theme.palette.textSecondary
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.preferredHeight: contentHeight
            interactive: false
            clip: true
            spacing: 0
            model: root.requiredPackagesModel

            delegate: PackageRowDelegate {
                width: ListView.view.width
                height: 56

                appRow: model
                installing: d.installing

                onVersionPicked: function(rowName, newVersion) {
                    var nextPicks = Object.assign({}, d.pickedVersions)
                    nextPicks[rowName] = newVersion
                    d.pickedVersions = nextPicks

                    root.versionChangeRequested(
                        d.targetName, d.targetRepoUrl, d.buildVersionPins())
                }
            }
        }

        // ─── Footer summary ───
        // On failure, show the error here; falls back to a generic line if the
        // backend didn't carry a reason.
        LogosText {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.topMargin: Theme.spacing.medium
            Layout.bottomMargin: Theme.spacing.large
            visible: root.installStage === InstallStage.Failed
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.error
            text: root.installError.length > 0
                  ? qsTr("Install failed: %1").arg(root.installError)
                  : qsTr("Install failed. Please try again.")
        }
        LogosText {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.topMargin: Theme.spacing.medium
            Layout.bottomMargin: Theme.spacing.large
            visible: root.installStage !== InstallStage.Failed
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textTertiary
            text: qsTr("%1 of %2 packages installed. Missing packages will be downloaded "
                       + "and installed as part of this install.")
                  .arg(d.installedDepsCount).arg(d.totalDeps)
        }
    }
}
