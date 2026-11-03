import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Icons
import Logos.Theme
import Basecamp.Backend 1.0
import Basecamp.Common

Dialog {
    id: root

    property var metadata: ({})
    property var requiredPackagesModel: null
    property int installStage: InstallStage.None
    property string installError: ""
    property var trust: ({})

    signal installRequested(string name, string repositoryUrl)
    signal trustAndInstallRequested(string name, string repositoryUrl)
    signal launchRequested(string name)
    signal versionChangeRequested(string name, string repositoryUrl, var versionPins, var optionalSelection, var optionalVersionPins)
    signal uninstallRequested(string name, string repositoryUrl)

    function openWith(metadata_) {
        root.metadata = metadata_ || ({})
        d.pickedVersions = ({})
        d.optionalSelection = ({})
        d.optionalPickedVersions = ({})
        root.installStage = root.metadata.installStage || InstallStage.None
        root.installError = ""   // clear any stale error from a prior open
        root.trust = root.metadata.trust || ({})
        open()
    }

    function refreshPreview() {
        root.metadata = Object.assign({}, root.metadata, {resolutionPending: true})
        root.versionChangeRequested(d.targetName, d.targetRepoUrl, d.buildVersionPins(),
                                    d.optionalSelection, d.optionalPickedVersions)
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
        property var optionalSelection: ({})
        property var optionalPickedVersions: ({})
        // The backend's default (`selected`) until the user toggles the row.
        function isOptionalSelected(p) {
            if (p.error || p.requiredFor) return false
            if (p.installedOnly) return true
            const own = d.optionalSelection[p.name]
            return own !== undefined ? own : p.selected !== false
        }
        readonly property var optionalPackages: root.metadata.optionalPackages || []
        onOptionalPackagesChanged: {
            // Required-version changes resolve a new optional set. Keep a
            // user's choice only while that version is still permitted.
            var picks = {}
            d.optionalPackages.forEach(function(p) {
                const picked = d.optionalPickedVersions[p.name]
                if (!p.error && picked && (picked === p.version
                    || (p.versions || []).some(function(v) {
                        return v && v.manifest && v.manifest.version === picked
                    })))
                    picks[p.name] = picked
            })
            d.optionalPickedVersions = picks
        }

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
        readonly property int tileSize: 64

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
            case InstallStatus.Installed:
                return root.metadata.optionalChangesPending ? "install" : "launch"
            }
            return "install"
        }
        readonly property bool trustApplies: d.actionMode === "install" || d.actionMode === "update"
                                             || d.actionMode === "downgrade" || d.actionMode === "reinstall"
        readonly property bool trustPending: d.trustApplies && root.trust.pending === true
        readonly property bool trustBlocked: d.trustApplies && root.trust.blocked === true
        readonly property bool trustAsks: d.trustApplies && !d.trustPending && !d.trustBlocked
                                          && root.trust.needsPrompt === true
        readonly property bool canTrustPublishers: d.trustAsks && root.trust.canTrust === true
        // Going on without trusting is going on "anyway", unless trusting
        // would settle everything that asks.
        readonly property bool anyway: d.trustAsks && root.trust.trustCoversAll !== true
        // A package's verdict (its InstallTrustLine or InstallTrustMark):
        // only for the plan on screen, once checked.
        function verdictFor(name) {
            return d.trustApplies && !d.trustPending && !root.metadata.resolutionPending
                   && root.trust.byName ? root.trust.byName[name] : undefined
        }
        // About the plan on screen, not one being resolved; while checking,
        // the button says so.
        readonly property bool showTrust: (d.trustAsks || d.trustBlocked)
                                          && !root.metadata.resolutionPending

        readonly property string actionText: {
            if ((root.metadata.resolutionPending || d.trustPending) && !d.installing
                && d.actionMode !== "launch")
                return qsTr("Checking packages…")
            switch (d.actionMode) {
            case "installing": return d.stageLabel
            case "install":    return d.anyway ? qsTr("Install anyway") : qsTr("Install")
            case "update":     return d.anyway ? qsTr("Update anyway") : qsTr("Update")
            case "downgrade":  return d.anyway ? qsTr("Downgrade anyway") : qsTr("Downgrade")
            case "reinstall":  return d.anyway ? qsTr("Reinstall anyway") : qsTr("Reinstall")
            case "launch":     return qsTr("Launch")
            }
            return ""
        }

        // Uninstall affordance. Keyed on installedVersion, NOT `installed` —
        // the latter is `installStatus === Installed`, which goes false the
        // moment an upgrade is available even though the package is very much
        // on disk. Embedded packages and main_ui are structurally protected.
        readonly property string installType: root.metadata.installType || ""
        readonly property bool canUninstall:
            d.installedVersion.length > 0
            && !d.installing
            && d.installType !== "embedded"
            && d.targetName !== "main_ui"

        readonly property bool actionEnabled:
            d.targetName.length > 0
            && !d.installing
            && d.installingBuckets === 0
            && (d.actionMode === "launch"
                || (!root.metadata.resolutionPending && !d.hasResolutionErrors
                    && !d.trustPending && !d.trustBlocked))

        readonly property int totalDeps:
            root.requiredPackagesModel ? root.requiredPackagesModel.visibleCount : 0
        readonly property int installedDepsCount:
            root.requiredPackagesModel ? root.requiredPackagesModel.installedCount : 0
        readonly property bool hasResolutionErrors:
            root.requiredPackagesModel ? root.requiredPackagesModel.hasResolutionErrors : false
        readonly property string counterText:
            d.installedDepsCount + " / " + d.totalDeps

        // ── Fixed section heights ──
        readonly property int showcaseHeight:   200
        readonly property int versionRowHeight: 52

        // ── Package list: the required rows, then the optional ones ──
        readonly property int depsRowHeight: 56
        readonly property int depsMaxRows:   6
        readonly property int depsWantedHeight: d.depsRowHeight * d.totalDeps
        readonly property int optionalHeadingHeight: 40
        readonly property int optionalWantedHeight: d.optionalPackages.length > 0
            ? d.optionalHeadingHeight + d.depsRowHeight * d.optionalPackages.length : 0
        readonly property int listWantedHeight: d.depsWantedHeight + d.optionalWantedHeight
        readonly property int listCapHeight: d.depsRowHeight * d.depsMaxRows
            + (d.optionalPackages.length > 0 ? d.optionalHeadingHeight + 3 * d.depsRowHeight : 0)

        readonly property int installFreshBuckets:
            root.requiredPackagesModel ? root.requiredPackagesModel.installFreshCount : 0
        readonly property int upgradeBuckets:
            root.requiredPackagesModel ? root.requiredPackagesModel.upgradeCount : 0
        readonly property int reinstallBuckets:
            root.requiredPackagesModel ? root.requiredPackagesModel.reinstallCount : 0
        readonly property int alreadyInstalledBuckets:
            root.requiredPackagesModel ? root.requiredPackagesModel.alreadyInstalledCount : 0
        readonly property int installingBuckets:
            root.requiredPackagesModel ? root.requiredPackagesModel.installingCount : 0
        readonly property int errorBuckets:
            root.requiredPackagesModel ? root.requiredPackagesModel.errorCount : 0
        readonly property int changingBuckets:
            d.installFreshBuckets + d.upgradeBuckets + d.reinstallBuckets

        function buildFooterText() {
            if (d.installing) {
                if (d.changingBuckets > 0)
                    return qsTr("%1 %2 package(s)…")
                        .arg(d.stageLabel).arg(d.changingBuckets)
                return d.stageLabel
            }

            if (d.installingBuckets > 0 && d.changingBuckets === 0) {
                return qsTr("%1 of %2's required package(s) are currently being installed. "
                            + "Wait for the active install to finish before launching %2.")
                    .arg(d.installingBuckets).arg(d.targetDisplayName)
            }

            // Top-level + deps all match disk → nothing to do. The action
            // button reads "Launch"; hide the footer entirely.
            if (d.changingBuckets === 0) return ""

            // Single-package (no separate deps): direct sentence tied to the
            // top-level's actionMode.
            if (d.totalDeps <= 1) {
                switch (d.actionMode) {
                case "install":
                    return qsTr("%1 will be installed.").arg(d.targetDisplayName)
                case "update":
                    return d.installedVersion.length > 0 && d.targetVersion.length > 0
                        ? qsTr("%1 will be updated from %2 to %3.")
                            .arg(d.targetDisplayName).arg(d.installedVersion).arg(d.targetVersion)
                        : qsTr("%1 will be updated.").arg(d.targetDisplayName)
                case "downgrade":
                    return d.installedVersion.length > 0 && d.targetVersion.length > 0
                        ? qsTr("%1 will be downgraded from %2 to %3.")
                            .arg(d.targetDisplayName).arg(d.installedVersion).arg(d.targetVersion)
                        : qsTr("%1 will be downgraded.").arg(d.targetDisplayName)
                case "reinstall":
                    return qsTr("%1 will be reinstalled.").arg(d.targetDisplayName)
                }
                return ""
            }

            // Multi-package
            const missing  = d.installFreshBuckets
            const outdated = d.upgradeBuckets
            const broken   = d.reinstallBuckets

            let stateText, actionText
            if (outdated > 0 && missing === 0 && broken === 0) {
                stateText  = qsTr("have become outdated")
                actionText = qsTr("please update your dependencies")
            } else if (missing > 0 && outdated === 0 && broken === 0) {
                stateText  = qsTr("are missing")
                actionText = qsTr("please install your dependencies")
            } else if (broken > 0 && outdated === 0 && missing === 0) {
                stateText  = qsTr("need to be reinstalled")
                actionText = qsTr("please reinstall your dependencies")
            } else {
                stateText  = qsTr("need updates")
                actionText = qsTr("please update your dependencies")
            }

            return qsTr("%1 out of %2 required package dependencies %3, "
                        + "%4 to be able to use the %5 application.")
                .arg(d.changingBuckets)
                .arg(d.totalDeps)
                .arg(stateText)
                .arg(actionText)
                .arg(d.targetDisplayName)
        }
        readonly property string footerText: buildFooterText()

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
    height: root.parent
            ? Math.min(implicitHeight, root.parent.height - 2 * Theme.spacing.large)
            : implicitHeight
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
            Layout.minimumHeight: implicitHeight
            Layout.fillHeight: false
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
                objectName: "addApplicationDialog.closeButton"
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
            objectName: "addApplicationDialog.showcase"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.preferredHeight: d.showcaseHeight
            Layout.minimumHeight: d.showcaseHeight
            color: Theme.palette.background
            radius: Theme.spacing.radiusMedium

            ColumnLayout {
                anchors.centerIn: parent
                spacing: Theme.spacing.small

                LogosTile {
                    Layout.alignment: Qt.AlignHCenter
                    label: d.targetDisplayName || d.targetName
                    source: d.targetIcon
                    fallbackColor: d.installed
                                   ? Theme.palette.surfaceRaised
                                   : AppColors.colorForApp(d.targetName)
                    tileSize: d.tileSize
                    interactive: false
                }

                LogosText {
                    objectName: "addApplicationDialog.name"
                    Layout.alignment: Qt.AlignHCenter
                    text: d.targetDisplayName
                    font.pixelSize: Theme.typography.primaryText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }

                InstallTrustLine {
                    objectName: "addApplicationDialog.trust." + d.targetName
                    Layout.alignment: Qt.AlignHCenter
                    verdict: d.verdictFor(d.targetName)
                }
            }
        }

        // ─── Description ───
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumHeight: implicitHeight
            Layout.fillHeight: false
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

        // ─── Version row: version | date | size | Install (| Trust & install) ───
        // The answers go under the version when they don't fit beside it.
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.preferredHeight: Math.max(d.versionRowHeight,
                                             versionGrid.implicitHeight + Theme.spacing.medium)
            Layout.minimumHeight: Layout.preferredHeight
            color: Theme.palette.background
            radius: Theme.spacing.radiusMedium

            GridLayout {
                id: versionGrid
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.spacing.medium
                anchors.rightMargin: Theme.spacing.small
                columns: versionLabel.implicitWidth + answers.implicitWidth + columnSpacing <= width ? 2 : 1
                columnSpacing: Theme.spacing.large
                rowSpacing: Theme.spacing.small

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    LogosText {
                        id: versionLabel
                        Layout.alignment: Qt.AlignVCenter
                        text: d.targetVersion ? ("Version " + d.targetVersion) : ""
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.secondaryText
                        font.weight: Theme.typography.weightMedium
                    }
                    // Where the buttons need the room, these give it up first.
                    LogosText {
                        objectName: "addApplicationDialog.releasedText"
                        Layout.alignment: Qt.AlignVCenter
                        Layout.minimumWidth: 0
                        text: d.releasedText
                        color: Theme.palette.textTertiary
                        font.pixelSize: Theme.typography.secondaryText
                        elide: Text.ElideRight
                    }
                    LogosText {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.minimumWidth: 0
                        text: d.sizeText
                        color: Theme.palette.textTertiary
                        font.pixelSize: Theme.typography.secondaryText
                        elide: Text.ElideRight
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    id: answers
                    objectName: "addApplicationDialog.answers"
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    spacing: Theme.spacing.large

                    LogosButton {
                        id: uninstallButton
                        objectName: "addApplicationDialog.uninstallButton"
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredHeight: 40
                        Layout.leftMargin: Theme.spacing.small
                        Layout.rightMargin: Theme.spacing.small
                        visible: d.canUninstall
                        text: qsTr("Uninstall")
                        onClicked: {
                            root.uninstallRequested(d.targetName, d.targetRepoUrl)
                            root.close()
                        }
                        contentItem: LogosText {
                            text: uninstallButton.text
                            color: Theme.palette.error
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Item {}
                    }

                    LogosButton {
                        id: actionButton
                        objectName: "addApplicationDialog.primaryButton"
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: Math.max(110, implicitWidth)
                        Layout.minimumWidth: Layout.preferredWidth
                        Layout.preferredHeight: 40
                        enabled: d.actionEnabled
                        text: d.actionText
                        onClicked: {
                            if (d.actionMode === "launch") {
                                root.launchRequested(d.targetName)
                                root.close()
                                return
                            }
                            root.installRequested(d.targetName, d.targetRepoUrl)
                        }
                        variant: d.canTrustPublishers ? LogosButton.Variant.Secondary
                                                      : LogosButton.Variant.Primary
                    }

                    LogosButton {
                        objectName: "addApplicationDialog.trustAndInstallButton"
                        visible: d.canTrustPublishers
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: Math.max(110, implicitWidth)
                        Layout.minimumWidth: Layout.preferredWidth
                        Layout.preferredHeight: 40
                        enabled: d.actionEnabled
                        // "anyway" too when trusting leaves something unsigned or unchecked.
                        text: {
                            switch (d.actionMode) {
                            case "update":    return d.anyway ? qsTr("Trust & update anyway") : qsTr("Trust & update")
                            case "downgrade": return d.anyway ? qsTr("Trust & downgrade anyway") : qsTr("Trust & downgrade")
                            case "reinstall": return d.anyway ? qsTr("Trust & reinstall anyway") : qsTr("Trust & reinstall")
                            }
                            return d.anyway ? qsTr("Trust & install anyway") : qsTr("Trust & install")
                        }
                        onClicked: root.trustAndInstallRequested(d.targetName, d.targetRepoUrl)
                        variant: LogosButton.Variant.Primary
                    }
                }
            }
        }

        // ─── Required Packages section ───
        RowLayout {
            Layout.fillWidth: true
            Layout.minimumHeight: implicitHeight
            Layout.fillHeight: false
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

        LogosListView {
            id: packageList
            objectName: "addApplicationDialog.requiredPackages"

            // Full-bleed, unlike every other section: the scrollbar is an
            // overlay painted at the view's right edge, so insetting the view
            // parks it 20px into the dialog, on top of each row's stage badge.
            // The 16px moves onto the rows instead (below), which leaves the
            // bar hugging the dialog edge and the content clear of it.
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Math.min(d.listWantedHeight, d.listCapHeight)
            Layout.maximumHeight: Layout.preferredHeight
            Layout.minimumHeight: d.depsRowHeight

            interactive: d.listWantedHeight > height
            spacing: 0
            model: root.requiredPackagesModel

            delegate: PackageRowDelegate {
                width: ListView.view.width
                height: d.depsRowHeight

                // The dialog's content margin, carried by the row rather than
                // the view. Padding moves the contentItem only, so the row's
                // separator rule still runs edge to edge.
                leftPadding: Theme.spacing.large
                rightPadding: Theme.spacing.large

                appRow: model
                installing: d.installing
                trustVerdict: d.verdictFor(model.name)

                onVersionPicked: function(rowName, newVersion) {
                    var nextPicks = Object.assign({}, d.pickedVersions)
                    nextPicks[rowName] = newVersion
                    d.pickedVersions = nextPicks

                    root.refreshPreview()
                }
            }

            footer: Item {
                objectName: "addApplicationDialog.optionalPackages"
                width: packageList.width
                visible: d.optionalPackages.length > 0
                height: d.optionalWantedHeight

                LogosText {
                    x: Theme.spacing.large
                    width: parent.width - 2 * Theme.spacing.large
                    height: d.optionalHeadingHeight
                    verticalAlignment: Text.AlignBottom
                    bottomPadding: Theme.spacing.small
                    text: qsTr("Optional Packages")
                    font.pixelSize: Theme.typography.panelTitleText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }

                Repeater {
                    model: d.optionalPackages
                    delegate: PackageRowDelegate {
                        required property var modelData
                        required property int index
                        objectName: "addApplicationDialog.optionalRow." + modelData.name
                        y: d.optionalHeadingHeight + index * d.depsRowHeight
                        width: packageList.width
                        height: d.depsRowHeight
                        // A package that comes only with an optional sits under it, without a checkbox.
                        leftPadding: Theme.spacing.large + (modelData.requiredFor ? 24 + Theme.spacing.small : 0)
                        rightPadding: Theme.spacing.large
                        appRow: modelData
                        installing: d.installing
                        selectable: !modelData.requiredFor
                        selectionObjectName: "addApplicationDialog.optional." + modelData.name
                        selected: d.isOptionalSelected(modelData)
                        selectedVersion: d.optionalPickedVersions[modelData.name] || ""
                        trustVerdict: d.verdictFor(modelData.name)
                        onSelectionToggled: function(checked) {
                            var selected = Object.assign({}, d.optionalSelection)
                            selected[modelData.name] = checked
                            d.optionalSelection = selected
                            root.refreshPreview()
                        }
                        onVersionPicked: function(name, version) {
                            var picks = Object.assign({}, d.optionalPickedVersions)
                            picks[name] = version
                            d.optionalPickedVersions = picks
                            root.refreshPreview()
                        }
                    }
                }
            }
        }

        // ─── Who signed it: the summary under the rows ───
        ColumnLayout {
            objectName: "addApplicationDialog.trust"
            visible: d.showTrust
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.topMargin: Theme.spacing.medium
            spacing: Theme.spacing.small

            InstallTrustNotice {
                Layout.fillWidth: true
                trust: d.showTrust ? root.trust : ({})
            }
        }

        // ─── Footer summary ───
        // On failure, show the error here; falls back to a generic line if the
        // backend didn't carry a reason.
        LogosText {
            objectName: "addApplicationDialog.errorText"
            Layout.fillWidth: true
            Layout.minimumHeight: implicitHeight
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
        // Resolver couldn't resolve one or more deps. Surfaced as a
        // warning-tone line
        LogosText {
            objectName: "addApplicationDialog.resolutionBanner"
            Layout.fillWidth: true
            Layout.minimumHeight: implicitHeight
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.topMargin: Theme.spacing.medium
            Layout.bottomMargin: Theme.spacing.large
            visible: root.installStage !== InstallStage.Failed && d.hasResolutionErrors
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.warning
            text: qsTr("Some required packages could not be resolved. "
                       + "Adjust versions or check your repositories.")
        }

        // Steady-state breakdown of what this install will do.
        LogosText {
            objectName: "addApplicationDialog.footerText"
            Layout.fillWidth: true
            Layout.minimumHeight: implicitHeight
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.topMargin: Theme.spacing.medium
            Layout.bottomMargin: Theme.spacing.large
            visible: root.installStage !== InstallStage.Failed
                     && !d.hasResolutionErrors
                     && d.footerText.length > 0
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textTertiary
            text: d.footerText
        }
    }
}
