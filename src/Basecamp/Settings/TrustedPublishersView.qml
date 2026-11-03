import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Icons
import Logos.Theme

Item {
    id: root
    objectName: "trustedPublishersView"

    property var publishers: []
    property var installedFrom: function(did) { return [] }

    signal removeRequested(string keyName)

    // A delete that failed; the publisher is as it was.
    function reportFailure(message) {
        failureNotice.message = message
        failureNotice.shown = true
    }

    QtObject {
        id: d
        property var pendingRemove: ({})
        property var pendingApps: []
        readonly property string pendingLabel: pendingRemove.label || qsTr("this publisher")

        // One pass, unlike chained arg(): a name that reads "%2" stays as it is.
        function fill(template, values) {
            return template.replace(/%([1-9])/g, (match, n) => n <= values.length ? values[n - 1] : match)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.spacing.large
        anchors.bottomMargin: Theme.spacing.large
        spacing: Theme.spacing.medium

        InspectorPanelHeader {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            Layout.minimumWidth: 0

            title: qsTr("Trusted Publishers")
            subtitle: qsTr("Packages these publishers signed install without asking; the rest ask first.")
            showReload: false
            visibleCount: list.count
            totalCount: list.count
        }

        LogosNotice {
            id: failureNotice
            objectName: "trustedPublishers.failure"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            severity: LogosNotice.Error
            closable: true
            shown: false
        }

        LogosText {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacing.large
            Layout.rightMargin: Theme.spacing.large
            text: qsTr("A publisher's name is its own claim: what you trust is its key, shown by its "
                       + "fingerprint. Deleting a publisher only changes future installs: what you "
                       + "installed keeps working.")
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textTertiary
            wrapMode: Text.WordWrap
        }

        LogosListView {
            id: list
            objectName: "trustedPublishers.list"

            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            model: root.publishers

            delegate: Item {
                id: row

                required property var modelData

                readonly property string keyName: modelData.name || ""
                readonly property string label: modelData.displayName || modelData.name
                                                || qsTr("Unnamed publisher")
                readonly property bool builtIn: modelData.builtIn === true
                // Being deleted.
                readonly property bool busy: modelData.busy === true
                readonly property string trustedSince: {
                    const added = new Date(modelData.addedAt || "")
                    return !isNaN(added.getTime())
                           ? qsTr("trusted since %1").arg(added.toLocaleDateString(Qt.locale(), "d MMM yyyy"))
                           : ""
                }

                width: ListView.view.width
                height: 56

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacing.large
                    anchors.rightMargin: Theme.spacing.large
                    spacing: Theme.spacing.small

                    LogosTile {
                        label: row.label
                        tileSize: 32
                        interactive: false
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        LogosText {
                            objectName: "trustedPublishers.name." + row.keyName
                            Layout.fillWidth: true
                            text: row.label
                            font.pixelSize: Theme.typography.primaryText
                            font.weight: Theme.typography.weightMedium
                            color: Theme.palette.text
                            elide: Text.ElideRight
                        }
                        LogosText {
                            objectName: "trustedPublishers.detail." + row.keyName
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: row.builtIn ? qsTr("Built in")
                                              : [row.modelData.url || "", row.trustedSince]
                                                    .filter(part => part.length > 0).join(" · ")
                            font.pixelSize: Theme.typography.secondaryText
                            color: Theme.palette.textTertiary
                            elide: Text.ElideRight
                        }
                    }

                    LogosText {
                        objectName: "trustedPublishers.fingerprint." + row.keyName
                        text: row.modelData.fingerprint || qsTr("unreadable key")
                        font.family: Theme.typography.mono
                        font.pixelSize: Theme.typography.secondaryText
                        color: Theme.palette.textSecondary
                    }
                    LogosCopyButton {
                        objectName: "trustedPublishers.copy." + row.keyName
                        value: row.modelData.did || ""
                        size: 20
                        iconSize: 14
                    }

                    Item {
                        Layout.leftMargin: Theme.spacing.large
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32

                        LogosIconButton {
                            objectName: "trustedPublishers.remove." + row.keyName
                            anchors.centerIn: parent
                            enabled: !row.busy
                            size: 32
                            iconSize: 18
                            iconSource: LogosIcons.trash
                            flat: true
                            Accessible.name: qsTr("Delete")
                            onClicked: {
                                d.pendingRemove = { keyName: row.keyName, label: row.label }
                                d.pendingApps = root.installedFrom(row.modelData.did) || []
                                removeDialog.open()
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Theme.palette.borderSubtle
                }
            }
        }
    }

    LogosWarningDialog {
        id: removeDialog
        objectName: "trustedPublishers.removeConfirm"

        anchors.centerIn: parent
        width: 420
        title: qsTr("Delete this publisher?")

        contentItem: LogosText {
            objectName: "trustedPublishers.removeConfirm.message"
            text: {
                let message = d.fill(qsTr("Future installs from “%1” will ask first."), [d.pendingLabel])
                if (d.pendingApps.length > 0)
                    message += " " + d.fill(qsTr("Apps you installed from it keep working: %1."),
                                               [d.pendingApps.join(", ")])
                return message + " " + qsTr("To trust it again, choose “Trust & install” when one of "
                                            + "its packages asks.")
            }
            color: Theme.palette.textSecondary
            font.pixelSize: Theme.typography.secondaryText
            wrapMode: Text.WordWrap
        }

        leftActions: [
            LogosButton {
                objectName: "trustedPublishers.removeConfirm.cancel"
                text: qsTr("Cancel")
                onClicked: {
                    d.pendingRemove = ({})
                    removeDialog.close()
                }
            }
        ]

        rightActions: [
            LogosButton {
                objectName: "trustedPublishers.removeConfirm.confirm"
                text: qsTr("Delete")
                variant: LogosButton.Variant.Primary
                onClicked: {
                    failureNotice.shown = false
                    root.removeRequested(d.pendingRemove.keyName || "")
                    d.pendingRemove = ({})
                    removeDialog.close()
                }
            }
        ]
    }
}
