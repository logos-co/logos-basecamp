import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

RowLayout {
    id: root

    // trust.byName[package], or undefined while that is checked.
    property var verdict

    visible: !!root.verdict && !!root.verdict.status
    spacing: Theme.spacing.tiny

    InstallTrustMark {
        id: mark
        objectName: "installTrust.lineMark"
        Layout.alignment: Qt.AlignTop
        verdict: root.verdict
        showsTooltip: false
    }

    LogosText {
        objectName: "installTrust.lineWords"
        Layout.fillWidth: true
        text: mark.description
        font.pixelSize: Theme.typography.secondaryText
        color: Theme.palette.textSecondary
        wrapMode: Text.WordWrap
    }
}
