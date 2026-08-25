import QtQuick
import QtQuick.Controls
import Logos.Controls
import Logos.Theme

// Shared chrome for the two intent consent prompts.
//
// Exists because both were carrying an identical header override and the same
// three geometry lines. The header is the reason: LogosDialog titles at 14px
// bold, which is right for an incidental dialog and reads as an afterthought on
// a prompt asking the user to hand one app a capability. ConfirmationDialog —
// the shell's other "are you sure" surface — titles at panelTitleText bold, and
// these belong with that.
//
// Escape only. An outside click must not answer a question an app is blocked on.
LogosDialog {
    id: root

    header: LogosText {
        text: root.title
        color: Theme.palette.text
        font.pixelSize: Theme.typography.panelTitleText
        font.weight: Theme.typography.weightBold
        leftPadding: Theme.spacing.large
        topPadding: Theme.spacing.large
        rightPadding: Theme.spacing.large
        bottomPadding: Theme.spacing.small
        wrapMode: Text.Wrap
        visible: root.title.length > 0
    }

    anchors.centerIn: parent
    width: 480
    closePolicy: Popup.CloseOnEscape
}
