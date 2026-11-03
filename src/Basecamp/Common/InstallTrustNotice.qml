import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

ColumnLayout {
    id: root

    // {pending: true} while checking; then {needsPrompt, blocked, packages,
    // publishers, byName}.
    property var trust: ({})

    visible: d.pending || d.needsPrompt
    spacing: Theme.spacing.small
    Layout.fillHeight: false

    QtObject {
        id: d

        readonly property bool pending: root.trust.pending === true
        readonly property bool blocked: root.trust.blocked === true
        readonly property bool needsPrompt: root.trust.needsPrompt === true
        readonly property var publishers: root.trust.publishers || []
        readonly property var packages: root.trust.packages || []
        readonly property string invalidNames: d.names(p => p.status === "invalid" && !p.reserved)
        readonly property string reservedNames: d.names(p => p.reserved === true)
        // Signed by someone else than the copy they replace, or replacing one
        // nobody signed.
        readonly property var replaced: d.packages.filter(p => !!p.replaces || p.replacesUnverified === true)
        // Whom "Trust & install" would trust, when it is offered.
        readonly property var trusting: root.trust.canTrust === true && !d.blocked ? (root.trust.trust || {}) : null

        function names(test) {
            return d.packages.filter(test).map(p => p.name).join(", ")
        }

        function fill(template, values) {
            return template.replace(/%([1-9])/g, (match, n) => n <= values.length ? values[n - 1] : match)
        }

        function describe(publisher) {
            const key = publisher.fingerprint || qsTr("unreadable")
            const signed = (publisher.packages || []).join(", ")
            if (publisher.impersonates)
                return d.fill(qsTr("Claims to be “%1”, but its key (%2) is not %1's: %3"),
                              [publisher.impersonates, key, signed])
            const name = publisher.name ? d.fill(qsTr("“%1”"), [publisher.name]) : qsTr("Unnamed publisher")
            return d.fill(qsTr("%1 · key %2 · %3"), [name, key, signed])
        }
    }

    LogosText {
        objectName: "installTrust.checking"
        visible: d.pending
        Layout.fillWidth: true
        text: qsTr("Checking who signed this…")
        font.pixelSize: Theme.typography.secondaryText
        color: Theme.palette.textSecondary
    }

    LogosNotice {
        objectName: "installTrust.warning"
        Layout.fillWidth: true
        shown: !d.pending && d.needsPrompt
        severity: d.blocked || d.replaced.length > 0 || d.publishers.some(p => p.impersonates)
                  ? LogosNotice.Error : LogosNotice.Warning
        title: d.blocked ? qsTr("This can't be installed.")
                         : d.replaced.length > 0 ? qsTr("Something here changes who signed it.")
                                                 : qsTr("Not everything here is from a publisher you trust.")
        message: {
            const lines = []
            if (!d.blocked)
                lines.push(qsTr("Install only if you trust its source."))
            if (d.trusting) {
                const whom = d.trusting.name ? d.fill(qsTr("“%1”"), [d.trusting.name])
                                             : d.fill(qsTr("key %1"), [d.trusting.fingerprint || qsTr("unreadable")])
                lines.push(d.fill(qsTr("Trusting %1 lets what it signs install without asking from now on."), [whom]))
            }
            for (let i = 0; i < d.publishers.length; ++i)
                lines.push(d.describe(d.publishers[i]))
            for (let i = 0; i < d.replaced.length; ++i)
                lines.push(d.replaced[i].replaces
                           ? d.fill(qsTr("%1 is not signed by %2, who signed the version you have."),
                                    [d.replaced[i].name, d.replaced[i].replaces])
                           : d.fill(qsTr("%1: the version you have isn't signed, so this may not be from the same publisher."),
                                    [d.replaced[i].name]))
            if (d.reservedNames.length > 0)
                lines.push(d.fill(qsTr("Comes with Basecamp, so only Logos can update it: %1"), [d.reservedNames]))
            if (d.invalidNames.length > 0)
                lines.push(d.fill(qsTr("Damaged, or the signature doesn't hold: %1"), [d.invalidNames]))
            return lines.join("\n")
        }
    }
}
