import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme
import Basecamp.Common
import Basecamp.Installer

ItemDelegate {
    id: root

    // ─── Public API ───
    property var appRow: ({})
    property bool selectable: false
    property bool selected: false
    property string selectedVersion: ""
    property string selectionObjectName: ""
    property var trustVerdict
    signal versionPicked(string name, string newVersion)
    signal selectionToggled(bool checked)

    QtObject {
        id: d

        readonly property string rowName: root.appRow ? (root.appRow.name || "") : ""
        readonly property string rowDisplayName:
            root.appRow ? (root.appRow.displayName || root.appRow.name || "") : ""
        readonly property string action: {
            const versions = root.appRow ? (root.appRow.versions || []) : []
            for (var i = 0; i < versions.length; ++i)
                if (versions[i] && versions[i].manifest && versions[i].manifest.version === d.toVersion && versions[i].action)
                    return versions[i].action
            return root.appRow ? (root.appRow.action || "") : ""
        }
        readonly property string toVersion: root.selectedVersion
            || (root.appRow ? (root.appRow.toVersion || root.appRow.version || "") : "")
        readonly property bool   isError: d.action === "error" || !!(root.appRow && root.appRow.error)
        readonly property bool   isInstalled:
            root.appRow ? (root.appRow.isInstalled === true) : false

        // Not the versions the download source cannot serve: the resolver
        // never picks one.
        readonly property var usableVersions:
            root.appRow ? (root.appRow.versions || []).filter(function(v) {
                return !v || v.sourceAvailable !== false
            }) : []

        // The action the resolver picked, as a RowAction.
        readonly property int rowAction: {
            switch (d.action) {
            case "install":   return Types.RowAction.Install
            case "upgrade":   return Types.RowAction.Upgrade
            case "downgrade": return Types.RowAction.Downgrade
            case "reinstall": return Types.RowAction.Reinstall
            case "installed": return Types.RowAction.NoOp
            }
            return d.isInstalled ? Types.RowAction.NoOp : Types.RowAction.Install
        }
    }

    hoverEnabled: true
    padding: 0
    autoExclusive: false

    background: Rectangle {
        color: "transparent"
        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: Theme.size.borderWidth
            color: Theme.palette.borderSubtle
        }
    }

    contentItem: RowLayout {
        spacing: Theme.spacing.medium

        RowLayout {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 150
            Layout.minimumWidth: Theme.size.touchTarget * 2
            Layout.maximumWidth: 150
            spacing: Theme.spacing.small

            LogosCheckbox {
                objectName: root.selectionObjectName
                visible: root.selectable
                Layout.preferredWidth: Theme.size.controlXSmall
                Layout.preferredHeight: Theme.size.controlXSmall
                // Installed-only (not in the catalog): kept as installed, nothing to choose.
                enabled: !d.isError && !(root.appRow && root.appRow.installedOnly)
                checked: root.selected
                onToggled: root.selectionToggled(checked)
                ToolTip.visible: hovered && !!root.appRow.error
                ToolTip.text: root.appRow.error || ""
            }

            LogosText {
                id: nameLabel
                objectName: "packageRow.name." + d.rowName
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: Math.ceil(implicitWidth)
                text: d.rowDisplayName
                font.weight: Theme.typography.weightMedium
                font.pixelSize: Theme.typography.primaryText
                color: Theme.palette.text
                elide: Text.ElideRight

                HoverHandler { id: nameHover }
                LogosToolTip {
                    text: d.rowDisplayName
                    placement: LogosToolTip.Top
                    visible: nameHover.hovered && nameLabel.truncated
                }
            }

            InstallTrustMark {
                objectName: "packageRow.trust." + d.rowName
                verdict: root.trustVerdict
            }

            Item { Layout.fillWidth: true }
        }

        // Per-row version picker. Same combo as before; emits a
        // versionPicked signal that the dialog folds into its pin map.
        Item {
            Layout.alignment: Qt.AlignVCenter
            Layout.fillWidth: true
            Layout.preferredWidth: 110
            Layout.minimumWidth: Theme.size.touchTarget * 2
            Layout.maximumWidth: 110
            Layout.preferredHeight: Theme.size.controlSmall

            LogosComboBox {
                id: versionCombo
                objectName: "packageRow.version." + d.rowName
                anchors.fill: parent
                visible: d.usableVersions.length > 0
                enabled: !d.isError
                // Catalog entries can have manifest: null; guard every access
                // or v.manifest.version throws a QML TypeError and blanks the picker.
                model: d.usableVersions.map(function(v) { return (v && v.manifest) ? (v.manifest.version || "") : "" })
                currentIndex: {
                    const list = d.usableVersions
                    for (var i = 0; i < list.length; ++i)
                        if (list[i] && list[i].manifest && list[i].manifest.version === d.toVersion) return i
                    return 0
                }
                displayText: {
                    const v = d.usableVersions[currentIndex]
                    return (v && v.manifest && v.manifest.version) ? ("v." + v.manifest.version) : ""
                }
                onActivated: function(idx) {
                    const list = d.usableVersions
                    if (idx < 0 || idx >= list.length) return
                    const picked = (list[idx] && list[idx].manifest) ? (list[idx].manifest.version || "") : ""
                    if (!picked || picked === d.toVersion) return
                    root.versionPicked(d.rowName, picked)
                }

                hoverEnabled: true
                LogosToolTip {
                    text: versionCombo.displayText
                    placement: LogosToolTip.Top
                    visible: versionCombo.hovered && versionCombo.contentLabel.truncated
                }
            }

            LogosText {
                id: versionFallback
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: d.usableVersions.length === 0
                text: d.toVersion ? ("v." + d.toVersion) : ""
                font.pixelSize: Theme.typography.secondaryText
                color: Theme.palette.textTertiary
                elide: Text.ElideRight

                HoverHandler { id: versionFallbackHover }
                LogosToolTip {
                    text: versionFallback.text
                    placement: LogosToolTip.Top
                    visible: versionFallbackHover.hovered && versionFallback.truncated
                }
            }
        }

        LogosText {
            id: descLabel
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.alignment: Qt.AlignVCenter
            objectName: "packageRowDescription"
            text: root.appRow ? (root.appRow.description || "") : ""
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textTertiary
            elide: Text.ElideRight
            opacity: width >= Theme.size.touchTarget ? 1 : 0

            HoverHandler { id: descHover }
            LogosToolTip {
                text: descLabel.text
                placement: LogosToolTip.Top
                visible: descHover.hovered && descLabel.truncated
            }
        }

        LogosBadge {
            id: stageBadge
            objectName: "packageRowStageBadge"
            Layout.alignment: Qt.AlignVCenter
            text: {
                if (d.isError) return root.selectable ? qsTr("Unavailable") : qsTr("Conflict")
                return Stages.actionWord(d.rowAction)
            }
            color: {
                if (d.isError) return Theme.palette.error
                switch (d.action) {
                case "install":   return Theme.palette.primary
                case "upgrade":   return Theme.palette.info
                case "downgrade": return Theme.palette.info
                case "reinstall": return Theme.palette.info
                case "installed": return Theme.palette.textTertiary
                }
                return d.isInstalled ? Theme.palette.textTertiary : Theme.palette.primary
            }
            radius: Theme.spacing.radiusLarge

            HoverHandler { id: badgeHover; enabled: d.isError }
            ToolTip.visible: badgeHover.hovered && d.isError
            ToolTip.text: (root.appRow && root.appRow.resolverError)
                              ? root.appRow.resolverError
                              : qsTr("Cannot resolve this dependency")
        }
    }
}
