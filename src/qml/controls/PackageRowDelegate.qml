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
    property var appRow: ({})
    property bool installing: false
    signal versionPicked(string name, string newVersion)

    QtObject {
        id: d

        readonly property string rowName: root.appRow ? (root.appRow.name || "") : ""
        readonly property string action:  root.appRow ? (root.appRow.action || "") : ""
        readonly property string toVersion: root.appRow ? (root.appRow.toVersion || "") : ""
        readonly property bool   isError:   d.action === "error"
        readonly property int rowStage:
            root.appRow && root.appRow.installStage !== undefined
                ? root.appRow.installStage
                : InstallStage.None

        readonly property var usableVersions:
            root.appRow ? (root.appRow.versions || []) : []
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
            height: 1
            color: Theme.palette.borderSubtle
        }
    }

    contentItem: RowLayout {
        spacing: Theme.spacing.medium

        LogosText {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 110
            text: d.rowName
            font.weight: Theme.typography.weightMedium
            font.pixelSize: Theme.typography.primaryText
            color: Theme.palette.text
            elide: Text.ElideRight
        }

        // Per-row version picker. Same combo as before; emits a
        // versionPicked signal that the dialog folds into its pin map.
        Item {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 110
            Layout.preferredHeight: 32

            LogosComboBox {
                anchors.fill: parent
                visible: d.usableVersions.length > 0
                enabled: !root.installing && !d.isError
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
            }

            LogosText {
                anchors.verticalCenter: parent.verticalCenter
                visible: d.usableVersions.length === 0
                text: d.toVersion ? ("v." + d.toVersion) : ""
                font.pixelSize: Theme.typography.secondaryText
                color: Theme.palette.textTertiary
            }
        }

        LogosText {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: root.appRow ? (root.appRow.description || "") : ""
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textTertiary
            elide: Text.ElideRight
        }

        LogosBadge {
            Layout.alignment: Qt.AlignVCenter
            text: {
                if (d.isError) return qsTr("Conflict")
                switch (d.rowStage) {
                case InstallStage.Downloading: return qsTr("Downloading…")
                case InstallStage.Queued:      return qsTr("Queued")
                case InstallStage.Installing:  return qsTr("Installing…")
                case InstallStage.Installed:   return qsTr("Installed")
                case InstallStage.Failed:      return qsTr("Failed")
                }
                switch (d.action) {
                case "install":   return qsTr("Install")
                case "upgrade":   return qsTr("Upgrade")
                case "downgrade": return qsTr("Downgrade")
                case "reinstall": return qsTr("Reinstall")
                default:          return qsTr("Installed")
                }
            }
            color: {
                if (d.isError) return Theme.palette.error
                switch (d.rowStage) {
                case InstallStage.Downloading:
                case InstallStage.Queued:
                case InstallStage.Installing:  return Theme.palette.warning
                case InstallStage.Installed:   return Theme.palette.textTertiary
                case InstallStage.Failed:      return Theme.palette.error
                }
                switch (d.action) {
                case "install":   return Theme.palette.primary
                case "upgrade":   return Theme.palette.info
                case "downgrade": return Theme.palette.info
                case "reinstall": return Theme.palette.info
                default:          return Theme.palette.textTertiary
                }
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
