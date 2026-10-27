import QtQuick

import Logos.Theme
import Logos.Controls

// Over the content area while offline, the notice at its bottom centre.
// MainContainer masks the widget to `toastRect`, so the rest of the content
// stays clickable. Closing hides it until the connection drops again or a
// while has passed.
Item {
    id: root

    signal dismissed()

    readonly property rect toastRect: Qt.rect(notice.x, notice.y, notice.width, notice.height)

    LogosNotice {
        id: notice
        objectName: "basecamp.offlineToast"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacing.large
        width: Math.min(parent.width - 2 * Theme.spacing.large, 560)
        severity: LogosNotice.Warning
        closable: true
        title: qsTr("You're offline")
        message: qsTr("Apps can't be downloaded until you're connected to the internet. Apps "
                      + "you already have still work.")
        onDismissed: {
            // Hidden by MainContainer from here; shown again next time.
            notice.shown = true
            root.dismissed()
        }
    }
}
