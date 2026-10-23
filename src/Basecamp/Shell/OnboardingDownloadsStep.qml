import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

// Advanced setup: where apps are downloaded from. Pick any of the two.
ColumnLayout {
    id: root

    property bool github: true
    property bool storage: true

    readonly property bool valid: github || storage
    // package_downloader's download source.
    readonly property string source: github && storage ? "any"
                                   : storage ? "logos"
                                   : github ? "http" : ""
    readonly property string hint: valid ? "" : qsTr("Pick at least one to continue")

    spacing: Theme.spacing.medium

    LogosText {
        text: qsTr("Configure how you download apps")
        color: Theme.palette.text
        font.pixelSize: 18
        font.weight: Theme.typography.weightBold
    }

    LogosText {
        Layout.fillWidth: true
        Layout.topMargin: -Theme.spacing.small
        text: qsTr("Please check the options you'd like.")
        color: Theme.palette.textSecondary
        font.pixelSize: Theme.typography.secondaryText
        wrapMode: Text.WordWrap
    }

    OnboardingChoiceCard {
        objectName: "onboardingGithubCard"
        multi: true
        title: qsTr("GitHub")
        description: qsTr("Download apps over the internet from GitHub. Fast, and works on "
                          + "any network.")
        selected: root.github
        onPicked: root.github = !root.github
    }

    OnboardingChoiceCard {
        objectName: "onboardingStorageCard"
        multi: true
        title: qsTr("Logos Storage")
        description: qsTr("Download apps from the Logos peer-to-peer network. Runs a storage "
                          + "node on this computer.")
        selected: root.storage
        onPicked: root.storage = !root.storage
    }

    LogosText {
        Layout.fillWidth: true
        text: qsTr("You can always change this later in Settings → Repositories.")
        color: Theme.palette.textTertiary
        font.pixelSize: 11
        wrapMode: Text.WordWrap
    }

    Item { Layout.fillHeight: true }
}
