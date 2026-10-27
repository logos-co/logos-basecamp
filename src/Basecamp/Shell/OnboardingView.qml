pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

Item {
    id: root

    // --- Inputs from the host ---

    property string storageState: ""
    property string storageError: ""
    property url backdrop
    property url logo

    // --- Outputs ---

    // The download source is chosen: "any", "logos" or "http".
    signal sourceChosen(string source)
    signal retryStorageRequested()
    signal finished()

    QtObject {
        id: d

        property string source: ""
        property int stepIndex: -1

        readonly property var steps: downloadsStep.storage ? ["downloads", "storage"]
                                                           : ["downloads"]

        readonly property string step: (stepIndex >= 0 && stepIndex < steps.length)
            ? steps[stepIndex]
            : "welcome"

        readonly property var stepNames: steps.map(function (name) {
            switch (name) {
            case "downloads": return qsTr("Downloads")
            case "storage":   return qsTr("Logos Storage")
            default:          return name
            }
        })

        function choose(source) {
            if (source === d.source) return
            d.source = source
            root.sourceChosen(source)
        }

        function startStorageStep() {
            storageStep.enter()
            d.stepIndex = d.steps.indexOf("storage")
        }

        // Nothing is final until the last step: going back and choosing again
        // just changes the source.
        readonly property bool canGoBack: step !== "welcome"

        function back() {
            d.stepIndex = d.stepIndex - 1
        }

        function advance() {
            switch (step) {
            case "downloads":
                d.choose(downloadsStep.source)
                if (downloadsStep.storage)
                    d.startStorageStep()
                else
                    root.finished()
                break
            case "storage":
                root.finished()
                break
            }
        }

        readonly property bool canAdvance: {
            switch (step) {
            case "downloads": return downloadsStep.valid
            case "storage":   return storageStep.done
            default:          return true
            }
        }

        readonly property string advanceHint: step === "downloads" ? downloadsStep.hint : ""

        readonly property string advanceText: {
            switch (step) {
            case "downloads":
                return downloadsStep.storage ? qsTr("Continue") : qsTr("Start Basecamp")
            case "storage":
                if (!storageStep.done)
                    return qsTr("Starting…")
                return storageStep.failed && !storageStep.githubAllowed
                    ? qsTr("Continue without downloads")
                    : qsTr("Start Basecamp")
            default:
                return qsTr("Continue")
            }
        }
    }

    // One page at a time: the welcome screen, then each setup step.
    StackLayout {
        objectName: "onboardingPages"
        anchors.fill: parent
        currentIndex: {
            switch (d.step) {
            case "downloads": return 1
            case "storage":   return 2
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

        OnboardingStepPage {
            objectPrefix: "onboardingStorage"
            stepNames: d.stepNames
            stepIndex: d.stepIndex
            canGoBack: d.canGoBack
            canAdvance: d.canAdvance
            advanceText: d.advanceText
            advanceHint: d.advanceHint
            onBackRequested: d.back()
            onAdvanceRequested: d.advance()
            secondaryText: storageStep.failed ? qsTr("Retry") : ""
            onSecondaryRequested: storageStep.retry()

            OnboardingStorageStep {
                id: storageStep
                objectName: "onboardingStorageStep"
                anchors.fill: parent
                source: d.source
                storageState: root.storageState
                storageError: root.storageError
                onRetryRequested: root.retryStorageRequested()
            }
        }
    }
}
