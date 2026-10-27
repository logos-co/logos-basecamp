import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls
import Logos.Icons

ColumnLayout {
    id: root

    // The chosen download source: "any" or "logos".
    property string source: "any"
    // package_downloader's storage state and error, from the backend.
    property string storageState: ""
    property string storageError: ""
    property int timeoutMs: 90000

    readonly property bool running: storageState === "running"
    // Held until a retry: falling back to GitHub turns the state "off".
    readonly property bool failed: d.gaveUp && !running
    readonly property bool done: running || failed
    readonly property bool githubAllowed: source !== "logos"
    readonly property string status: running ? "running" : (failed ? "failed" : "starting")

    signal retryRequested()

    function enter() {
        if (d.entered) return
        d.entered = true
        begin()
    }

    function retry() {
        begin()
        retryRequested()
    }

    function begin() {
        d.started = Date.now()
        d.now = d.started
        d.timedOut = false
        d.gaveUp = false
        d.lastError = ""
    }

    // Kept: falling back to GitHub clears it.
    onStorageErrorChanged: if (storageError.length > 0) d.lastError = storageError

    spacing: Theme.spacing.medium

    QtObject {
        id: d

        property real started: Date.now()
        property real now: Date.now()
        property bool timedOut: false
        property bool gaveUp: false
        property bool entered: false
        readonly property bool failing: root.storageState === "failed" || d.timedOut

        property string lastError: ""
        readonly property string reason: d.timedOut
            ? qsTr("It did not start within %1 seconds.").arg(Math.round(root.timeoutMs / 1000))
            : (d.lastError.length > 0 ? d.lastError : qsTr("Logos Storage reported a failure."))

        onFailingChanged: if (d.failing) d.gaveUp = true

        readonly property string elapsed: {
            const seconds = Math.max(0, Math.floor((d.now - d.started) / 1000))
            return Math.floor(seconds / 60) + ":" + String(seconds % 60).padStart(2, "0")
        }
    }

    Timer {
        interval: 1000
        repeat: true
        running: root.visible && !root.done
        onTriggered: {
            d.now = Date.now()
            if (d.now - d.started >= root.timeoutMs) {
                d.timedOut = true
                d.gaveUp = true
            }
        }
    }

    LogosText {
        text: qsTr("Logos Storage")
        color: Theme.palette.text
        font.pixelSize: 18
        font.weight: Theme.typography.weightBold
    }

    LogosText {
        Layout.fillWidth: true
        Layout.topMargin: -Theme.spacing.small
        text: qsTr("Basecamp downloads apps through a storage node on this computer. It "
                   + "usually starts in under a minute.")
        color: Theme.palette.textSecondary
        font.pixelSize: Theme.typography.secondaryText
        wrapMode: Text.WordWrap
    }

    RowLayout {
        objectName: "onboardingStorageStatus"
        Layout.fillWidth: true
        Layout.topMargin: Theme.spacing.small
        visible: !root.failed
        spacing: Theme.spacing.medium

        Item {
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            Layout.alignment: Qt.AlignTop

            LogosSpinner {
                objectName: "onboardingStorageSpinner"
                anchors.fill: parent
                visible: !root.running
                running: visible
                thickness: 2
                dotSize: 4
            }

            LogosIcon {
                objectName: "onboardingStorageRunningIcon"
                anchors.fill: parent
                visible: root.running
                source: LogosIcons.check
                color: Theme.palette.success
                brightness: 1.0
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing.small

                LogosText {
                    objectName: "onboardingStorageStatusText"
                    text: root.running ? qsTr("Logos Storage is running")
                                       : qsTr("Starting Logos Storage…")
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.primaryText
                    font.weight: Theme.typography.weightBold
                }

                LogosText {
                    objectName: "onboardingStorageElapsed"
                    visible: !root.done
                    text: d.elapsed
                    color: Theme.palette.textTertiary
                    font.family: Theme.typography.mono
                    font.pixelSize: Theme.typography.secondaryText
                }

                Item { Layout.fillWidth: true }
            }

            LogosText {
                Layout.fillWidth: true
                visible: root.running
                text: qsTr("Apps can now be downloaded from the Logos network.")
                color: Theme.palette.textSecondary
                font.pixelSize: Theme.typography.secondaryText
                wrapMode: Text.WordWrap
            }
        }
    }

    LogosNotice {
        id: meanwhileNotice
        objectName: "onboardingStorageGithubMeanwhile"
        Layout.fillWidth: true
        shown: root.failed && root.githubAllowed
        severity: LogosNotice.Warning
        title: qsTr("Logos Storage couldn't start")
        message: qsTr("Apps download from GitHub meanwhile. Basecamp tries Logos Storage again "
                      + "each time it starts.")
        actions: [
            LogosCopyableText {
                objectName: "onboardingStorageMeanwhileReason"
                // The actions row is not a layout: wrap at the message's width.
                width: Math.min(implicitWidth, meanwhileNotice.messageItem.width)
                text: d.reason
                textColor: Theme.palette.textSecondary
                textItem.font.family: Theme.typography.mono
                textItem.font.pixelSize: Theme.typography.secondaryText
                textItem.wrapMode: TextEdit.Wrap
            }
        ]
    }

    LogosNotice {
        id: failedNotice
        objectName: "onboardingStorageFailed"
        Layout.fillWidth: true
        shown: root.failed && !root.githubAllowed
        severity: LogosNotice.Error
        title: qsTr("Logos Storage couldn't start")
        message: qsTr("Basecamp can't download apps until it does. Apps you already have still "
                      + "work. You can also allow GitHub downloads in Settings → Repositories.")
        actions: [
            LogosCopyableText {
                objectName: "onboardingStorageFailedReason"
                width: Math.min(implicitWidth, failedNotice.messageItem.width)
                text: d.reason
                textColor: Theme.palette.textSecondary
                textItem.font.family: Theme.typography.mono
                textItem.font.pixelSize: Theme.typography.secondaryText
                textItem.wrapMode: TextEdit.Wrap
            }
        ]
    }

    Item { Layout.fillHeight: true }
}
