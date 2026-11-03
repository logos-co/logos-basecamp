pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Dialogs

import Logos.Controls
import Logos.Theme

// Settings → Appearance. Pick a theme, add your own theme.json, or remove one
// you added
Item {
    id: root

    signal installRequested(url file)
    signal selectRequested(string themeId)
    signal removeRequested(string themeId)

    // `result` is Logos::ThemeManager::install()'s
    // { ok, id, name, errors, warnings }. A clean install is a toast; problems
    // stay on the page until dismissed, so they can be read.
    function showInstallResult(result) {
        if (result.ok && result.warnings.length === 0) {
            d.showToast(qsTr("Added “%1”").arg(result.name), qsTr("It's now your theme."))
        } else if (result.ok) {
            d.showNotice(LogosNotice.Warning,
                         qsTr("Added “%1”, but some of it was ignored").arg(result.name),
                         result.warnings.join("\n"))
        } else {
            d.showNotice(LogosNotice.Error, qsTr("That file isn't a valid theme"),
                         result.errors.join("\n"))
        }
    }

    // `result` is Logos::ThemeManager::remove()'s { ok, id, name, errors }.
    function showRemoveResult(result) {
        if (result.ok)
            d.showToast(qsTr("Removed “%1”").arg(result.name),
                        d.removedWasCurrent ? qsTr("Switched back to Dark.")
                                            : qsTr("It's no longer in your themes."))
        else
            d.showNotice(LogosNotice.Error, qsTr("Couldn't remove the theme"),
                         result.errors.join("\n"))
    }

    QtObject {
        id: d

        // Theme.themeInfo entries in Theme.themeNames order: Dark first.
        readonly property var themes: Theme.themeNames.map(id => Theme.themeInfo[id])

        // A theme's own colour for `token`, Dark's where the theme leaves it out.
        // Read per row for the swatch; Theme builds a full palette only for
        // the current theme.
        function themeColour(id, token) {
            const installed = ThemeRegistry.themes
            for (let i = 0; i < installed.length; ++i)
                if (installed[i].id === id && installed[i].palette[token] !== undefined)
                    return installed[i].palette[token]
            return Theme.darkTheme[token]
        }

        // The theme the remove dialog asks about.
        property var pendingRemove: null
        property bool removedWasCurrent: false

        property int resultSeverity: LogosNotice.Info
        property string resultTitle: ""
        property string resultMessage: ""

        function showNotice(severity, title, message) {
            resultSeverity = severity
            resultTitle = title
            resultMessage = message
        }

        function showToast(title, message) {
            resultTitle = ""
            toast.show(title, message)
        }
    }

    FileDialog {
        id: fileDialog
        objectName: "appearance.fileDialog"
        title: qsTr("Choose a theme file")
        modality: Qt.NonModal
        nameFilters: [qsTr("Logos theme (*.json)")]
        onAccepted: root.installRequested(selectedFile)
    }

    LogosScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: Theme.spacing.large
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: Theme.spacing.large

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.tiny

                LogosText {
                    text: qsTr("Appearance")
                    font.pixelSize: Theme.typography.panelTitleText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }
                LogosText {
                    text: qsTr("Choose a theme, or add your own theme file.")
                    font.pixelSize: Theme.typography.secondaryText
                    color: Theme.palette.textSecondary
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            // One row per theme: pick with the radio, remove the ones you added.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.maximumWidth: 640
                spacing: Theme.spacing.tiny

                Repeater {
                    model: d.themes
                    delegate: ThemeListDelegate {
                        id: themeRow
                        required property var modelData
                        Layout.fillWidth: true
                        themeId: themeRow.modelData.id
                        name: themeRow.modelData.name
                        author: themeRow.modelData.author
                        builtin: themeRow.modelData.builtin
                        current: themeRow.modelData.id === Theme.currentTheme
                        background: d.themeColour(themeRow.modelData.id, "background")
                        accent: d.themeColour(themeRow.modelData.id, "primary")
                        onSelectRequested: root.selectRequested(themeRow.themeId)
                        onRemoveRequested: {
                            d.pendingRemove = themeRow.modelData
                            removeDialog.open()
                        }
                    }
                }
            }

            LogosButton {
                objectName: "appearance.addButton"
                text: qsTr("Add custom theme…")
                variant: LogosButton.Variant.Primary
                onClicked: fileDialog.open()
            }

            // Errors and warnings quote keys from the file: keep them plain text.
            LogosNotice {
                objectName: "appearance.resultNotice"
                Layout.fillWidth: true
                Layout.maximumWidth: 560
                shown: d.resultTitle.length > 0
                closable: true
                severity: d.resultSeverity
                title: d.resultTitle
                message: d.resultMessage
                titleItem.textFormat: Text.PlainText
                messageItem.textFormat: Text.PlainText
                onDismissed: d.resultTitle = ""
            }
        }
    }

    LogosToast {
        id: toast
        objectName: "appearance.toast"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacing.large
        width: Math.min(parent.width - Theme.spacing.large * 2, 480)
        severity: LogosNotice.Success
    }

    LogosWarningDialog {
        id: removeDialog
        objectName: "appearance.removeDialog"
        anchors.centerIn: parent
        width: 420
        title: d.pendingRemove ? qsTr("Remove “%1”?").arg(d.pendingRemove.name) : ""
        message: d.pendingRemove && d.pendingRemove.id === Theme.currentTheme
                 ? qsTr("It's the theme in use, so Basecamp switches back to Dark. You can add it again from its file.")
                 : qsTr("You can add it again from its file.")

        leftActions: [
            LogosButton {
                objectName: "appearance.removeDialog.cancel"
                text: qsTr("Cancel")
                onClicked: removeDialog.close()
            }
        ]
        rightActions: [
            LogosButton {
                objectName: "appearance.removeDialog.confirm"
                text: qsTr("Remove")
                variant: LogosButton.Variant.Primary
                onClicked: {
                    d.removedWasCurrent = d.pendingRemove.id === Theme.currentTheme
                    root.removeRequested(d.pendingRemove.id)
                    removeDialog.close()
                }
            }
        ]
    }
}
