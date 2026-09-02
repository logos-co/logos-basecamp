import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Icons
import Logos.Theme

// Which app should handle this request?
//
// IT RETURNS A DECISION AND DOES NOT ROUTE — picking an entry calls back into
// the broker, which dispatches. That is what lets this be re-pointed at a
// runtime-supplied selection without touching anything that calls it.
//
// EVERY STRING HERE COMES FROM THE SHELL. `displayName` and `iconSource` are
// resolved by IntentRegistry through the same callables the sidebar uses, so a
// requesting app cannot influence how a provider is presented and nothing
// renderable crosses from the requester.
//
// Dismissal is a real answer: `cancelled`, distinct from `unavailable`, so an
// app can tell "the user said no" from "there was nobody to ask".
IntentDialog {
    id: root

    objectName: "intentChooserDialog"

    // ── Injected by the shell ───────────────────────────────────────────────
    property var displayNameLookup: function (name) { return name }
    property var detailsLookup:     function (name) { return ({}) }

    // ── The request on screen. Read-only: openWith() is the only way in, so
    //    the four fields cannot drift out of step with each other. ───────────
    readonly property alias dispatchId:    d.dispatchId
    readonly property alias intentName:    d.intentName
    readonly property alias requesterName: d.requesterName
    readonly property alias providers:     d.providers

    // moduleName of the row whose details are open, or "".
    readonly property alias expandedProvider: d.expanded

    function toggleDetails(moduleName) {
        d.expanded = (d.expanded === moduleName) ? "" : moduleName
    }

    // `dispatchId` is the BROKER's id for this request, never the requester's
    // own — that one never leaves its side.
    signal providerChosen(string dispatchId, string providerName)
    signal choiceCancelled(string dispatchId)

    // One object, taken whole: the previous positional form was
    // (dispatchId, intent, requester, providers), three of them strings, so
    // transposing intent and requester produced a plausible-looking prompt
    // describing the wrong app.
    function openWith(request) {
        var r = request || ({})
        d.dispatchId    = r.dispatchId    || ""
        d.intentName    = r.intent        || ""
        d.requesterName = r.requesterName || ""
        d.providers     = r.providers     || []
        d.expanded      = ""
        d.answered      = false
        open()
    }

    // A withdrawal for a request that already ended must not shut whatever has
    // since taken the slot.
    function closeFor(dispatchId) {
        if (dispatchId !== d.dispatchId)
            return
        d.answered = true
        close()
    }

    // Escape, or any Dialog-managed dismissal. Something is blocked on an
    // answer, so silence is not an option.
    onClosed: {
        if (d.answered) {
            d.answered = false
            return
        }
        root.choiceCancelled(d.dispatchId)
    }

    title: qsTr("Choose an app")

    QtObject {
        id: d

        property string dispatchId: ""
        property string intentName: ""
        property string requesterName: ""
        property var    providers: []

        // moduleName of the expanded row, or "". One at a time, so the dialog
        // cannot outgrow the screen on a long provider list.
        property string expanded: ""

        // Collapsed height of a provider row. The single number this layout is
        // built from — the header strip and the icon tile are both derived from
        // it, so changing it here moves everything together.
        readonly property int rowHeight: 56

        // Set before close() by anything that has already answered, so onClosed
        // does not send a second, contradictory `cancelled`.
        property bool answered: false

        // Human phrasing for the shell's own capabilities. Anything else falls
        // back to the raw name rather than inventing wording for a capability
        // the shell does not define.
        readonly property var intentLabels: ({
            "logos.repositories.manage": qsTr("manage package repositories")
        })

        readonly property string intentLabel:
            intentLabels[d.intentName] !== undefined ? intentLabels[d.intentName]
                                                     : d.intentName

        function answer(providerName) {
            d.answered = true
            root.providerChosen(d.dispatchId, providerName)
            root.close()
        }

        function cancel() {
            d.answered = true
            root.choiceCancelled(d.dispatchId)
            root.close()
        }

    }

    rightActions: [
        LogosButton {
            objectName: "intentChooserCancel"
            text: qsTr("Cancel")
            onClicked: d.cancel()
        }
    ]

    contentItem: ColumnLayout {
        spacing: Theme.spacing.medium

        LogosText {
            Layout.fillWidth: true
            text: root.displayNameLookup(d.requesterName)
                  + qsTr(" wants to ") + d.intentLabel + "."
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textSecondary
            wrapMode: Text.Wrap
        }

        // The display name above is whatever its author typed — 
        // a package called evil_ui may ship `"display_name": "Wallet"` 
        // — and this line is the only thing that says otherwise.
        LogosText {
            Layout.fillWidth: true
            text: d.requesterName
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textSubtle
            elide: Text.ElideRight
            visible: d.requesterName !== ""
                     && root.displayNameLookup(d.requesterName) !== d.requesterName
        }

        LogosListView {
            objectName: "intentChooserProviders"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 240)
            model: d.providers
            interactive: contentHeight > height

            delegate: LogosItemDelegate {
                id: row

                readonly property bool expanded: d.expanded === modelData.moduleName
                readonly property var facts:
                    row.expanded ? root.detailsLookup(modelData.moduleName) : ({})

                width: ListView.view.width
                objectName: "intentProvider_" + modelData.moduleName
                readonly property int headerHeight:
                    d.rowHeight - row.topPadding - row.bottomPadding

                implicitHeight: row.expanded ? d.rowHeight + facts_.implicitHeight
                                             : d.rowHeight
                radius: Theme.spacing.radiusSmall
                onClicked: d.answer(modelData.moduleName)

                contentItem: ColumnLayout {
                    spacing: Theme.spacing.tiny

                    RowLayout {
                        id: headerRow

                        Layout.fillWidth: true
                        Layout.preferredHeight: row.headerHeight
                        spacing: Theme.spacing.small

                        LogosTile {
                            objectName: "intentProviderIcon_" + modelData.moduleName

                            readonly property int side:
                                Math.max(20, Math.min(32, headerRow.height
                                                          - 2 * Theme.spacing.tiny))
                            Layout.preferredWidth: side
                            Layout.preferredHeight: side
                            Layout.alignment: Qt.AlignVCenter
                            label: modelData.displayName || modelData.moduleName
                            source: modelData.iconSource || ""
                            tileSize: side
                            interactive: false
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 0

                            LogosText {
                                Layout.fillWidth: true
                                text: modelData.displayName || modelData.moduleName
                                font.pixelSize: Theme.typography.primaryText
                                color: Theme.palette.text
                                elide: Text.ElideRight
                            }

                            LogosText {
                                Layout.fillWidth: true
                                text: modelData.moduleName
                                font.pixelSize: Theme.typography.secondaryText
                                color: Theme.palette.textSubtle
                                elide: Text.ElideRight
                                visible: (modelData.displayName || "") !== modelData.moduleName
                            }
                        }

                        LogosLink {
                            objectName: "intentProviderDetails_" + modelData.moduleName
                            text: row.expanded ? qsTr("Hide") : qsTr("Details")
                            underline: false
                            Layout.alignment: Qt.AlignVCenter
                            onActivated: root.toggleDetails(modelData.moduleName)
                        }
                    }

                    // What the user needs to judge "should this app get this
                    // capability", and nothing else. Every field is resolved
                    // shell-side; the provider contributes none of it.
                    ColumnLayout {
                        id: facts_

                        objectName: "intentProviderFacts_" + modelData.moduleName
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.spacing.medium
                        Layout.bottomMargin: Theme.spacing.small
                        visible: row.expanded
                        spacing: Theme.spacing.tiny

                        // Absorbs clicks, so reading the panel cannot choose the
                        // provider by accident. The row around it stays live.
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.AllButtons
                            onClicked: function (mouse) { mouse.accepted = true }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            Layout.bottomMargin: Theme.spacing.tiny
                            color: Theme.palette.border
                        }

                        LogosText {
                            Layout.fillWidth: true
                            text: row.facts.verified
                                ? qsTr("Signed by a trusted publisher")
                                : qsTr("Unsigned — the shell cannot confirm who published this")
                            font.pixelSize: Theme.typography.secondaryText
                            color: row.facts.verified ? Theme.palette.text
                                                      : Theme.palette.textSecondary
                            wrapMode: Text.Wrap
                        }

                        Repeater {
                            model: [
                                { label: qsTr("Package"), value: modelData.moduleName },
                                { label: qsTr("Version"), value: row.facts.version || "" },
                                { label: qsTr("From"),    value: row.facts.repositoryUrl || "" },
                                { label: qsTr("Install"), value: row.facts.installType || "" }
                            ]

                            delegate: RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.small
                                visible: modelData.value !== ""

                                LogosText {
                                    Layout.preferredWidth: 64
                                    text: modelData.label
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: Theme.palette.textSubtle
                                }

                                LogosText {
                                    Layout.fillWidth: true
                                    text: modelData.value
                                    font.pixelSize: Theme.typography.secondaryText
                                    color: Theme.palette.textSecondary
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        LogosText {
                            Layout.fillWidth: true
                            text: row.facts.description || ""
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSubtle
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                            visible: text !== ""
                        }
                    }
                }
            }
        }
    }
}
