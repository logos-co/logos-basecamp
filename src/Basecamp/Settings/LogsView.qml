import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Theme

import Basecamp.Backend
import Basecamp.Common

// Settings → Logs. Reads the per-session files LogRedirector writes under
// <baseDirectory>/logs and shows one file at a time: a file list on the
// left, the parsed lines on the right with level / source filter chips, the
// page-level search bar, and a follow switch that tails the file the running
// session is writing.
//
// Unlike the inspectors this view calls its backend directly: `logs` is the
// LogManager object (backend.logs), self-contained enough that routing every
// call through ContentViews would only add indirection.
Item {
    id: root
    objectName: "logsView"

    // ─── Public API ───
    property var logs: null
    property string searchText: ""

    function levelColor(level) {
        switch (level) {
        case "critical": return Theme.palette.error
        case "warning":  return Theme.palette.warning
        case "qml":      return Theme.palette.accentOrange
        case "info":     return Theme.palette.info
        case "out":      return Theme.palette.textSecondary
        default:         return Theme.palette.textTertiary
        }
    }

    function bytesText(bytes) {
        if (bytes >= 1024 * 1024 * 1024) return (bytes / (1024 * 1024 * 1024)).toFixed(1) + " GB"
        if (bytes >= 1024 * 1024)        return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        if (bytes >= 1024)               return (bytes / 1024).toFixed(0) + " KB"
        return bytes + " B"
    }

    // Programmatic filter entry points: "" clears; otherwise a level name or
    // a source name. Used by UI tests and meant for a Module Inspector → Logs
    // jump later.
    function setLevelFilter(value)  { d.applyLevel(value) }
    function setSourceFilter(value) { d.applySource(value) }

    QtObject {
        id: d
        property var levels: []     // empty = all
        property var sources: []    // empty = all
        readonly property int keepSessions: root.logs ? root.logs.defaultKeepSessions : 30

        // Dropdown state. `*Value` is the selected option's value; `levels` /
        // `sources` above are what the proxy actually filters on.
        property string levelValue: ""
        property string sourceValue: ""
        property var levelOptions: [{ label: qsTr("All levels"), value: "" }]
        property var sourceOptions: [{ label: qsTr("All sources"), value: "" }]
        property bool optionsDirty: false

        // The combos are created after `d`; bindings that fire during
        // construction (onLogsChanged) must not touch them yet. Flipped from
        // the root's Component.onCompleted.
        property bool combosReady: false

        function applyLevel(value) {
            levelValue = value
            levels = value === "" ? [] : [value]
            if (combosReady) levelBox.currentIndex = Math.max(0, levelBox.indexOfValue(value))
        }
        function applySource(value) {
            sourceValue = value
            sources = value === "" ? [] : [value]
            if (combosReady) sourceBox.currentIndex = Math.max(0, sourceBox.indexOfValue(value))
        }
        // Options are the names present in the open file (the header's
        // "N of M" carries the numbers). Only replaced when the set of names
        // changes, so Follow's per-second appends don't reset the combos.
        function sameValues(a, b) {
            if (a.length !== b.length) return false
            for (let i = 0; i < a.length; ++i) if (a[i].value !== b[i].value) return false
            return true
        }
        function rebuildOptions() {
            if (!combosReady) return
            if (levelBox.popupItem.visible || sourceBox.popupItem.visible) {
                optionsDirty = true
                return
            }
            optionsDirty = false
            const lv = [{ label: qsTr("All levels"), value: "" }]
            const sv = [{ label: qsTr("All sources"), value: "" }]
            if (root.logs) {
                const lc = root.logs.levelCounts
                for (let i = 0; i < lc.length; ++i)
                    lv.push({ label: lc[i].name, value: lc[i].name })
                const sc = root.logs.sourceCounts
                for (let i = 0; i < sc.length; ++i)
                    sv.push({ label: sc[i].name, value: sc[i].name })
            }
            if (!sameValues(lv, levelOptions))  levelOptions = lv
            if (!sameValues(sv, sourceOptions)) sourceOptions = sv
        }

        // Selection is a contiguous range of *visible* rows (proxy indices):
        // click selects one, shift+click extends from the anchor. It is
        // dropped whenever the rows it points at could have moved: a new
        // file, a filter change, a reload.
        property int selAnchor: -1
        property int selFrom: -1
        property int selTo: -1
        readonly property bool hasSelection: selFrom >= 0 && selTo >= selFrom

        function select(row, extend) {
            if (extend && selAnchor >= 0) {
                selFrom = Math.min(selAnchor, row)
                selTo = Math.max(selAnchor, row)
                return
            }
            if (selFrom === row && selTo === row) { clearSelection(); return }
            selAnchor = row
            selFrom = row
            selTo = row
        }
        function clearSelection() { selAnchor = -1; selFrom = -1; selTo = -1 }

        function copySelection() {
            if (!hasSelection || !root.logs) return
            const n = filtered.lineCount(selFrom, selTo)
            root.logs.copyText(filtered.textForRows(selFrom, selTo))
            notice.show(qsTr("Copied %n line(s).", "", n))
        }
        function copyVisible() {
            if (!root.logs || filtered.visibleCount === 0) return
            const n = filtered.lineCount(0, filtered.visibleCount - 1)
            root.logs.copyText(filtered.textForRows(0, filtered.visibleCount - 1))
            notice.show(qsTr("Copied %n line(s).", "", n))
        }

        readonly property string openPath: root.logs ? root.logs.currentFile : ""
        onOpenPathChanged: clearSelection()
        onLevelsChanged: clearSelection()
        onSourcesChanged: clearSelection()
    }

    // ⌘C / Ctrl+C copies the selected rows unless the user is selecting text
    // inside the detail pane, where the TextEdit owns the shortcut.
    Shortcut {
        sequences: [StandardKey.Copy]
        context: Qt.WindowShortcut
        enabled: root.visible && d.hasSelection && !detailText.activeFocus
        onActivated: d.copySelection()
    }

    // Polling and the first file load only happen while this page is on
    // screen; StackLayout flips `visible` for us.
    onVisibleChanged: {
        if (logs) logs.active = visible
        // A selection left behind on a hidden page would keep the ⌘C
        // shortcut armed for a range the user can't see.
        if (!visible) d.clearSelection()
    }
    Component.onCompleted: {
        d.combosReady = true
        if (logs) logs.active = visible
        d.rebuildOptions()
    }
    onLogsChanged: { if (logs) logs.active = visible; d.rebuildOptions() }

    LogFilterProxy {
        id: filtered
        sourceModel: root.logs ? root.logs.lines : null
        levels:      d.levels
        sources:     d.sources
        searchText:  root.searchText

        onSearchTextChanged: d.clearSelection()
        onModelReset: d.clearSelection()
    }

    Connections {
        target: root.logs
        function onSessionsDeleted(deletedFiles, deletedSessions, error) {
            notice.show(error.length > 0
                ? error
                : qsTr("Deleted %n file(s)", "", deletedFiles)
                  + " " + qsTr("from %n session(s).", "", deletedSessions),
                error.length > 0)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.large
        spacing: Theme.spacing.medium

        InspectorPanelHeader {
            Layout.fillWidth: true
            Layout.minimumWidth: 0

            title: qsTr("Logs")
            subtitle: root.logs
                      ? qsTr("Output captured from Basecamp and its modules. %1 sessions, %2 in %3")
                            .arg(root.logs.sessionCount)
                            .arg(root.bytesText(root.logs.totalBytes))
                            .arg(root.logs.logsDirectory)
                      : ""
            reloadObjectName: "logs.reloadButton"
            loading: root.logs ? root.logs.loading : false
            visibleCount: filtered.visibleCount
            totalCount: filtered.totalCount

            onReloadClicked: if (root.logs) root.logs.reload()
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing.medium

            // ─── Files ───
            ColumnLayout {
                Layout.preferredWidth: 260
                Layout.minimumWidth: 220
                Layout.maximumWidth: 300
                Layout.fillHeight: true
                spacing: Theme.spacing.small

                LogosText {
                    text: qsTr("Files")
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightMedium
                    color: Theme.palette.text
                }

                LogosListView {
                    id: filesList
                    objectName: "logs.filesList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    model: root.logs ? root.logs.files : null

                    delegate: LogosItemDelegate {
                        id: fileCell
                        required property int index
                        required property string path
                        required property string sessionLabel
                        required property int rotation
                        required property int fileCount
                        required property double size
                        required property bool isLive
                        required property bool startsSession

                        readonly property bool isCurrent:
                            root.logs && path === root.logs.currentFile

                        width: ListView.view.width
                        highlighted: isCurrent
                        radius: Theme.spacing.radiusLarge
                        highlightColor: Theme.palette.backgroundButton
                        hoverColor: Theme.colors.getColor(Theme.palette.hover, 0.5)
                        implicitHeight: fileCol.implicitHeight + Theme.spacing.small * 2
                        text: ""
                        onClicked: if (root.logs) root.logs.openFile(path)

                        ColumnLayout {
                            id: fileCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: Theme.spacing.small
                            anchors.rightMargin: Theme.spacing.small
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.small
                                LogosText {
                                    Layout.fillWidth: true
                                    text: fileCell.startsSession
                                          ? fileCell.sessionLabel
                                          : "  ·  " + qsTr("file %1 of %2")
                                                        .arg(fileCell.rotation + 1)
                                                        .arg(fileCell.fileCount)
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: fileCell.startsSession
                                                 ? Theme.typography.weightMedium
                                                 : Theme.typography.weightRegular
                                    color: (fileCell.highlighted || fileCell.hovered)
                                           ? Theme.palette.text : Theme.palette.textSecondary
                                    elide: Text.ElideRight
                                }
                                LogosBadge {
                                    visible: fileCell.isLive
                                    text: qsTr("Live")
                                    color: Theme.palette.success
                                }
                            }
                            LogosText {
                                Layout.fillWidth: true
                                // Sessions that rotated say so on their heading row;
                                // single-file sessions just show the size.
                                text: (fileCell.startsSession && fileCell.fileCount > 1
                                       ? qsTr("file %1 of %2")
                                             .arg(fileCell.rotation + 1)
                                             .arg(fileCell.fileCount) + "  ·  " : "")
                                      + root.bytesText(fileCell.size)
                                font.pixelSize: Theme.typography.secondaryText
                                color: Theme.palette.textTertiary
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                LogosText {
                    visible: root.logs && root.logs.files.count === 0
                    Layout.fillWidth: true
                    text: qsTr("No log files yet.")
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small

                    LogosButton {
                        Layout.fillWidth: true
                        implicitHeight: 36
                        text: qsTr("Open folder")
                        onClicked: if (root.logs) root.logs.openLogsFolder()
                    }
                    LogosButton {
                        objectName: "logs.deleteOlderButton"
                        Layout.fillWidth: true
                        implicitHeight: 36
                        text: qsTr("Delete old…")
                        enabled: root.logs && root.logs.sessionCount > d.keepSessions
                        onClicked: deleteDialog.open()
                    }
                }
            }

            // ─── Lines ───
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.spacing.small

                // Filters: one dropdown per facet listing the values present in
                // the open file. Never rebuilt while a popup is open (Follow
                // appends every second; the menu must not jump under the
                // pointer).
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small

                    LogosText {
                        text: qsTr("Level")
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textTertiary
                    }
                    LogosComboBox {
                        id: levelBox
                        objectName: "logs.levelBox"
                        Layout.preferredWidth: 210
                        model: d.levelOptions
                        textRole: "label"
                        valueRole: "value"
                        onActivated: d.applyLevel(currentValue)
                        onModelChanged: {
                            const i = indexOfValue(d.levelValue)
                            if (i < 0) d.applyLevel("")
                            currentIndex = Math.max(0, indexOfValue(d.levelValue))
                        }
                    }

                    LogosText {
                        Layout.leftMargin: Theme.spacing.small
                        text: qsTr("Source")
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textTertiary
                    }
                    LogosComboBox {
                        id: sourceBox
                        objectName: "logs.sourceBox"
                        Layout.preferredWidth: 260
                        model: d.sourceOptions
                        textRole: "label"
                        valueRole: "value"
                        onActivated: d.applySource(currentValue)
                        onModelChanged: {
                            const i = indexOfValue(d.sourceValue)
                            if (i < 0) d.applySource("")
                            currentIndex = Math.max(0, indexOfValue(d.sourceValue))
                        }
                    }

                    Item { Layout.fillWidth: true }
                }

                Connections {
                    target: root.logs
                    function onCountsChanged() { d.rebuildOptions() }
                }
                Connections {
                    target: levelBox.popupItem
                    function onClosed() { if (d.optionsDirty) d.rebuildOptions() }
                }
                Connections {
                    target: sourceBox.popupItem
                    function onClosed() { if (d.optionsDirty) d.rebuildOptions() }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.medium

                    LogosText {
                        Layout.fillWidth: true
                        text: root.logs && root.logs.currentFileName.length > 0
                              ? root.logs.currentFileName : qsTr("No file selected")
                        font.family: root.logs ? root.logs.monospaceFamily : Theme.typography.mono
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textSecondary
                        elide: Text.ElideMiddle
                    }

                    LogosBadge {
                        visible: root.logs && root.logs.truncated
                        text: qsTr("Showing last 32 MB")
                        color: Theme.palette.warning
                    }

                    LogosButton {
                        objectName: "logs.copyVisibleButton"
                        implicitHeight: 36
                        text: qsTr("Copy visible")
                        enabled: filtered.visibleCount > 0
                        onClicked: d.copyVisible()
                    }

                    RowLayout {
                        spacing: Theme.spacing.small
                        visible: root.logs && root.logs.currentIsLive
                        LogosSwitch {
                            id: followSwitch
                            objectName: "logs.followSwitch"
                            checked: root.logs ? root.logs.following : false
                            onToggled: if (root.logs) root.logs.following = checked
                        }
                        LogosText {
                            text: qsTr("Follow")
                            color: Theme.palette.textSecondary
                            font.pixelSize: Theme.typography.secondaryText
                        }
                    }
                }

                // Error banner — same shape as RepositoriesView.
                Rectangle {
                    Layout.fillWidth: true
                    visible: root.logs && root.logs.error.length > 0
                    radius: Theme.spacing.radiusSmall
                    color: Theme.colors.getColor(Theme.palette.error, 0.12)
                    border.color: Theme.palette.error
                    border.width: 1
                    implicitHeight: errorText.implicitHeight + Theme.spacing.medium * 2

                    LogosText {
                        id: errorText
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        text: root.logs ? root.logs.error : ""
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.secondaryText
                        wrapMode: Text.WordWrap
                    }
                }

                // Transient confirmation (copied, deleted) — hides itself.
                Rectangle {
                    id: notice
                    property string text: ""
                    property bool isError: false
                    function show(message, error) {
                        text = message
                        isError = error === true
                        visible = true
                        noticeTimer.restart()
                    }
                    readonly property color tone: isError ? Theme.palette.error : Theme.palette.success

                    Layout.fillWidth: true
                    visible: false
                    radius: Theme.spacing.radiusSmall
                    color: Theme.colors.getColor(tone, 0.12)
                    border.color: tone
                    border.width: 1
                    implicitHeight: noticeText.implicitHeight + Theme.spacing.medium * 2

                    LogosText {
                        id: noticeText
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        text: notice.text
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.secondaryText
                        wrapMode: Text.WordWrap
                    }
                    Timer {
                        id: noticeTimer
                        interval: 3000
                        onTriggered: notice.visible = false
                    }
                }

                // The lines.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.backgroundInset
                    border.color: Theme.palette.borderSubtle
                    border.width: 1
                    clip: true

                    LogosListView {
                        id: linesList
                        objectName: "logs.linesList"
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.tiny
                        spacing: 0
                        model: filtered
                        currentIndex: -1

                        // Stick to the bottom while following; a manual scroll
                        // up pauses that until the user scrolls back down.
                        property bool atEnd: true
                        onContentYChanged: {
                            if (!moving && !flicking) return
                            atEnd = contentY + height >= contentHeight - 4
                        }
                        onCountChanged: {
                            if (root.logs && root.logs.following && atEnd)
                                Qt.callLater(positionViewAtEnd)
                        }
                        Connections {
                            target: root.logs
                            function onLoadingChanged() {
                                if (!root.logs.loading) {
                                    linesList.atEnd = true
                                    Qt.callLater(linesList.positionViewAtEnd)
                                }
                            }
                        }

                        delegate: Rectangle {
                            id: lineRow
                            required property int index
                            required property string timestamp
                            required property string level
                            required property string source
                            required property string message

                            readonly property bool selected:
                                d.hasSelection && index >= d.selFrom && index <= d.selTo

                            width: ListView.view.width
                            implicitHeight: 24
                            color: selected
                                   ? Theme.colors.getColor(Theme.palette.primary, 0.18)
                                   : (index % 2 === 0 ? "transparent"
                                                      : Theme.colors.getColor(Theme.palette.surface, 0.35))

                            MouseArea {
                                anchors.fill: parent
                                onClicked: function(mouse) {
                                    d.select(lineRow.index, (mouse.modifiers & Qt.ShiftModifier) !== 0)
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacing.small
                                anchors.rightMargin: Theme.spacing.small
                                spacing: Theme.spacing.small

                                LogosText {
                                    Layout.preferredWidth: 176
                                    text: lineRow.timestamp.length > 0 ? lineRow.timestamp : ""
                                    font.family: root.logs ? root.logs.monospaceFamily : Theme.typography.mono
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: Theme.palette.textTertiary
                                    elide: Text.ElideRight
                                }
                                LogosText {
                                    Layout.preferredWidth: 60
                                    text: lineRow.level
                                    font.family: root.logs ? root.logs.monospaceFamily : Theme.typography.mono
                                    font.pixelSize: Theme.typography.secondaryText
                                    font.weight: Theme.typography.weightMedium
                                    color: root.levelColor(lineRow.level)
                                    elide: Text.ElideRight
                                }
                                LogosText {
                                    Layout.preferredWidth: 150
                                    text: lineRow.source
                                    font.family: root.logs ? root.logs.monospaceFamily : Theme.typography.mono
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: lineRow.source === "basecamp"
                                           ? Theme.palette.textSecondary : Theme.palette.accentOrange
                                    elide: Text.ElideRight
                                }
                                LogosText {
                                    Layout.fillWidth: true
                                    text: lineRow.message
                                    font.family: root.logs ? root.logs.monospaceFamily : Theme.typography.mono
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: Theme.palette.text
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        LogosText {
                            anchors.centerIn: parent
                            visible: linesList.count === 0 && !(root.logs && root.logs.loading)
                            text: !root.logs || root.logs.currentFile.length === 0
                                  ? qsTr("Select a file to view its log.")
                                  : (filtered.totalCount === 0
                                     ? qsTr("This file is empty.")
                                     : qsTr("No lines match the current filter."))
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.primaryText
                        }
                    }

                    LoadingOverlay {
                        anchors.fill: parent
                        visible: root.logs && root.logs.loading
                    }
                }

                // The selected rows, as the file has them, selectable for
                // partial copies. Shows up to `previewLines`; Copy takes all.
                Rectangle {
                    id: detailPane
                    visible: d.hasSelection && root.logs
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(220, detailCol.implicitHeight + Theme.spacing.medium * 2)
                    radius: Theme.spacing.radiusLarge
                    color: Theme.palette.surface
                    border.color: Theme.palette.borderSubtle
                    border.width: 1
                    clip: true

                    readonly property int previewLines: 200
                    readonly property int lineCount: d.hasSelection ? filtered.lineCount(d.selFrom, d.selTo) : 0
                    readonly property string previewText:
                        d.hasSelection ? filtered.textForRows(d.selFrom, d.selTo, previewLines) : ""

                    ColumnLayout {
                        id: detailCol
                        anchors.fill: parent
                        anchors.margins: Theme.spacing.medium
                        spacing: Theme.spacing.small

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.small

                            LogosText {
                                Layout.fillWidth: true
                                text: detailPane.lineCount === 1
                                      ? qsTr("1 line selected")
                                      : qsTr("%n lines selected", "", detailPane.lineCount)
                                        + (detailPane.lineCount > detailPane.previewLines
                                           ? "  ·  " + qsTr("showing the first %1; Copy takes all").arg(detailPane.previewLines)
                                           : "  ·  " + qsTr("shift+click extends"))
                                font.pixelSize: Theme.typography.secondaryText
                                color: Theme.palette.textTertiary
                                elide: Text.ElideRight
                            }
                            LogosButton {
                                objectName: "logs.copySelectionButton"
                                implicitHeight: 36
                                variant: LogosButton.Variant.Primary
                                text: detailPane.lineCount === 1
                                      ? qsTr("Copy line")
                                      : qsTr("Copy %n lines", "", detailPane.lineCount)
                                onClicked: d.copySelection()
                            }
                            LogosIconButton {
                                flat: true
                                size: 36
                                iconSize: 14
                                iconSource: LogosIcons.close
                                onClicked: d.clearSelection()
                            }
                        }

                        LogosScrollView {
                            id: detailScroll
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 24
                            LogosSelectableText {
                                id: detailText
                                width: detailScroll.availableWidth
                                text: detailPane.previewText
                                font.family: root.logs ? root.logs.monospaceFamily : Theme.typography.mono
                                font.pixelSize: Theme.typography.secondaryText
                                wrapMode: TextEdit.WrapAnywhere
                            }
                        }
                    }
                }
            }
        }
    }

    LogosWarningDialog {
        id: deleteDialog
        anchors.centerIn: parent
        width: 440
        accentColor: Theme.palette.error
        title: qsTr("Delete older sessions?")
        message: root.logs
                 ? qsTr("Keeps the %1 most recent sessions (the running one always stays) and deletes the other %2. This cannot be undone.")
                       .arg(d.keepSessions)
                       .arg(Math.max(0, root.logs.sessionCount - d.keepSessions))
                 : ""

        leftActions: [
            LogosButton {
                text: qsTr("Cancel")
                onClicked: deleteDialog.close()
            }
        ]
        rightActions: [
            LogosButton {
                objectName: "logs.deleteConfirmButton"
                text: qsTr("Delete")
                variant: LogosButton.Variant.Primary
                onClicked: {
                    if (root.logs) root.logs.deleteOlderSessions(d.keepSessions)
                    deleteDialog.close()
                }
            }
        ]
    }
}
