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
        readonly property string monogram:      (d.nameText || "?").substring(0, 2).toUpperCase()
        readonly property bool   hasIcon:       d.iconUrl.length > 0
    }

    padding: 0
    hoverEnabled: true

    onClicked: root.appClicked(d.nameText, d.repositoryUrl)

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: root.manageRequested(d.nameText, d.repositoryUrl)
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        longPressThreshold: 0.6
        onLongPressed: root.manageRequested(d.nameText, d.repositoryUrl)
    }

    background: Rectangle {
        anchors.fill: parent
        anchors.margins: Theme.spacing.tiny
        radius: Theme.spacing.radiusLarge
        color: root.hovered ? Theme.palette.surfaceRaised : "transparent"
        opacity: d.isInstalled ? 1.0 : 0.7
    }

    contentItem: RowLayout {
        anchors.leftMargin: Theme.spacing.medium
        anchors.rightMargin: Theme.spacing.medium
        spacing: Theme.spacing.medium

        Rectangle {
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.spacing.radiusMedium
            color: Theme.palette.backgroundTertiary

            Image {
                anchors.centerIn: parent
                source: d.iconUrl
                sourceSize.width: 24
                sourceSize.height: 24
                visible: d.hasIcon
            }

            LogosText {
                anchors.centerIn: parent
                text: d.monogram
                font.pixelSize: Theme.typography.secondaryText
                font.weight: Theme.typography.weightMedium
                color: Theme.palette.textTertiary
                visible: !d.hasIcon
            }
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
            visible: d.isInstalling
                     || d.installStage === InstallStage.Failed
                     || !d.isInstalled
                     || d.hasUpdate
            text: d.isInstalling                          ? qsTr("Installing…")
                : d.installStage === InstallStage.Failed  ? qsTr("Failed")
                : d.hasUpdate                             ? qsTr("Update")
                                                          : qsTr("Install")
            color: d.isInstalling                          ? Theme.palette.warning
                 : d.installStage === InstallStage.Failed  ? Theme.palette.error
                 : d.hasUpdate                             ? Theme.palette.info
                                                           : Theme.palette.accentOrange
        }
    }
}
