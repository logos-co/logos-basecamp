pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

Item {
    id: root

    // --- Inputs from the host ---

    property url backdrop
    property url logo

    // --- Outputs ---

    // The download source is chosen: "any", "logos" or "http". The storage node
    // starts later, for the first download that needs it.
    signal sourceChosen(string source)
    signal finished()

    QtObject {
        id: d

        property string source: ""
        property int stepIndex: -1

        readonly property var steps: ["downloads"]

        readonly property string step: (stepIndex >= 0 && stepIndex < steps.length)
            ? steps[stepIndex]
            : "welcome"

        readonly property var stepNames: steps.map(function (name) {
            switch (name) {
            case "downloads": return qsTr("Downloads")
            default:          return name
            }
        })

        function choose(source) {
            if (source === d.source) return
            d.source = source
            root.sourceChosen(source)
        }

        readonly property bool canGoBack: step !== "welcome"

        function back() {
            d.stepIndex = d.stepIndex - 1
        }

        function advance() {
            switch (step) {
            case "downloads":
                d.choose(downloadsStep.source)
                root.finished()
                break
            }
        }

        readonly property bool canAdvance: {
            switch (step) {
            case "downloads": return downloadsStep.valid
            default:          return true
            }
        }

        readonly property string advanceHint: step === "downloads" ? downloadsStep.hint : ""

        readonly property string advanceText: step === "downloads" ? qsTr("Start Basecamp")
                                                                   : qsTr("Continue")
    }

    // One page at a time: the welcome screen, then each setup step.
    StackLayout {
        objectName: "onboardingPages"
        anchors.fill: parent
        currentIndex: {
            switch (d.step) {
            case "downloads": return 1
            default:          return 0
            }
        }

        OnboardingWelcome {
            objectName: "onboardingWelcome"
            backdrop: root.backdrop
            logo: root.logo
            onQuickSetupRequested: {
                d.choose("any")
                root.finished()
            }
            onCustomSetupRequested: d.stepIndex = 0
        }

        OnboardingStepPage {
            objectPrefix: "onboardingDownloads"
            stepNames: d.stepNames
            stepIndex: d.stepIndex
            canGoBack: d.canGoBack
            canAdvance: d.canAdvance
            advanceText: d.advanceText
            advanceHint: d.advanceHint
            onBackRequested: d.back()
            onAdvanceRequested: d.advance()

            OnboardingDownloadsStep {
                id: downloadsStep
                objectName: "onboardingDownloadsStep"
                anchors.fill: parent
            }
        }
    }
}
