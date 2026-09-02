import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Icons
import Logos.Theme

// An app asked for something nothing installed can do — but the catalog knows a
// package that could.
//
// A SUGGESTION, NOT A PENDING REQUEST. The request that prompted it was already
// answered `unavailable` — the same answer, on the same floor, as if no such
// package existed. An app that could tell those apart would have an oracle for
// what the user has *not* installed.
//
// So this never names the requester and never reports back. It carries no
// dispatch id, cannot be withdrawn, and installing resumes nothing: the user
// retries once the package lands.
IntentDialog {
    id: root

    objectName: "intentInstallDialog"

    property var displayNameLookup: function (name) { return name }

    // Injected for the same reason displayNameLookup is: this dialog resolves
    // nothing itself. Reaching into Basecamp.AppManager for AppColors would have
    // been the one place a Shell dialog imported a feature module for a value
    // rather than a type — and would drag the whole module into any test that
    // instantiates this component.
    property var fallbackColorFor: function (name) { return Theme.palette.surfaceRaised }

    readonly property alias intentName: d.intentName

    // [{ moduleName, displayName, repositoryUrl }], sorted, as the shell
    // supplied them. Previously two parallel arrays that had to agree by
    // position; one list cannot fall out of step with itself.
    readonly property alias candidates: d.candidates

    // Which one Install will act on. Readable so a caller can assert the
    // selection; changed only through select().
    readonly property alias selectedCandidate: d.selected

    signal installRequested(string providerName)

    function openWith(intent, candidates, details) {
        d.intentName = intent || ""
        d.candidates = d.merge(candidates || [], details || [])
        d.selected   = d.candidates.length > 0 ? d.candidates[0].moduleName : ""
        open()
    }

    function select(moduleName) {
        d.selected = moduleName
    }

    title: qsTr("Install an app for this?")

    QtObject {
        id: d

        // Collapsed height of a candidate row; the icon tile is derived from it.
        readonly property int rowHeight: 52

        property string intentName: ""
        property var    candidates: []
        property string selected: ""

        // The shell hands over names and details separately; join them here so
        // nothing downstream has to.
        function merge(names, details) {
            var byName = ({})
            for (var i = 0; i < details.length; ++i)
                byName[details[i].moduleName] = details[i]

            var out = []
            for (var j = 0; j < names.length; ++j) {
                var det = byName[names[j]] || ({})
                out.push({
                    moduleName:    names[j],
                    displayName:   det.displayName || "",
                    repositoryUrl: det.repositoryUrl || ""
                })
            }
            return out
        }

        // Hostname only. A full URL is unreadable in a list row, and the host is
        // the part that tells you whether this is the catalog you expect.
        function originOf(url) {
            if (!url)
                return ""
            var m = /^[a-z]+:\/\/([^\/]+)/.exec(url)
            return m ? m[1] : url
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing.medium

        // Deliberately does not name the requesting app. The user is being asked
        // about a capability and a package, not about who wanted it.
        LogosText {
            objectName: "intentInstallBody"
            Layout.fillWidth: true
            text: {
                if (d.candidates.length !== 1)
                    return qsTr("Nothing installed can handle this. Choose one to install:")

                var only = d.candidates[0]
                var origin = d.originOf(only.repositoryUrl)
                return root.displayNameLookup(only.moduleName)
                     + qsTr(" can handle this, but it is not installed yet.")
                     + (origin ? qsTr("\nIt would be installed from ") + origin + "." : "")
            }
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textSecondary
            wrapMode: Text.Wrap
        }

        LogosListView {
            objectName: "intentInstallCandidates"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 180)
            visible: d.candidates.length > 1
            model: d.candidates
            interactive: contentHeight > height

            delegate: LogosItemDelegate {
                id: row

                readonly property bool isSelected: d.selected === modelData.moduleName

                width: ListView.view.width
                objectName: "intentInstallCandidate_" + modelData.moduleName

                implicitHeight: d.rowHeight
                radius: Theme.spacing.radiusSmall
                highlighted: row.isSelected

                onClicked: root.select(modelData.moduleName)

                contentItem: RowLayout {
                    id: headerRow

                    spacing: Theme.spacing.small

                    LogosTile {
                        objectName: "intentInstallIcon_" + modelData.moduleName

                        // Derived from the strip it sits in, capped so a taller
                        // row cannot turn the icon into the row.
                        readonly property int side:
                            Math.max(20, Math.min(32, headerRow.height
                                                      - 2 * Theme.spacing.tiny))

                        Layout.preferredWidth: side
                        Layout.preferredHeight: side
                        Layout.alignment: Qt.AlignVCenter
                        label: root.displayNameLookup(modelData.moduleName)
                        fallbackColor: root.fallbackColorFor(modelData.moduleName)
                        tileSize: side
                        interactive: false
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 0

                        LogosText {
                            Layout.fillWidth: true
                            text: root.displayNameLookup(modelData.moduleName)
                            font.pixelSize: Theme.typography.primaryText
                            font.weight: row.isSelected ? Theme.typography.weightMedium
                                                        : Theme.typography.weightRegular
                            color: Theme.palette.text
                            elide: Text.ElideRight
                        }

                        LogosText {
                            objectName: "intentInstallSubtitle_" + modelData.moduleName
                            Layout.fillWidth: true
                            text: {
                                var origin = d.originOf(modelData.repositoryUrl)
                                return origin ? modelData.moduleName + " · " + origin
                                              : modelData.moduleName
                            }
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textSubtle
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        LogosText {
            Layout.fillWidth: true
            text: qsTr("You will be asked to confirm the install, then can try the action again.")
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textSubtle
            wrapMode: Text.Wrap
        }
    }

    rightActions: [
        LogosButton {
            objectName: "intentInstallCancel"
            text: qsTr("Not now")
            onClicked: root.close()
        },
        LogosButton {
            objectName: "intentInstallConfirm"
            text: qsTr("Install")
            variant: LogosButton.Variant.Primary
            enabled: d.selected !== ""
            onClicked: {
                root.installRequested(d.selected)
                root.close()
            }
        }
    ]
}
