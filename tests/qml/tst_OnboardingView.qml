import QtQuick
import QtTest

// Directory import: see tst_IntentChooserDialog.qml.
import "../../src/Basecamp/Shell"

// First-run onboarding, driven through its own buttons. What matters is which
// download source each route reports, and that a storage failure never changes
// it: Basecamp tries Storage again on its next start.
TestCase {
    id: testCase
    name: "OnboardingView"
    when: windowShown
    width: 1024
    height: 768

    Component {
        id: viewComp
        OnboardingView {
            width: testCase.width
            height: testCase.height
        }
    }

    Component {
        id: spyComp
        SignalSpy {}
    }

    // TestCase itself is invisible, and an invisible item takes no clicks: the
    // view goes into the window instead.
    function makeView() {
        const view = createTemporaryObject(viewComp, testCase.Window.window.contentItem)
        verify(view)
        return view
    }

    function spy(target, signalName) {
        return createTemporaryObject(spyComp, testCase, { target: target, signalName: signalName })
    }

    // A page is laid out only once it is the current one, on the next layout
    // pass: clicking before that hits where the item was, not where it is.
    function click(view, name) {
        const item = findChild(view, name)
        verify(item, name)
        verify(item.visible, name + " visible")
        waitForRendering(view)
        mouseClick(item)
    }

    // Each step page has its own footer: use the one on screen.
    function currentPrefix(view) {
        return findChild(view, "onboardingStorageStep").visible ? "onboardingStorage"
                                                                : "onboardingDownloads"
    }
    function advance(view) { click(view, currentPrefix(view) + "AdvanceButton") }
    function advanceButton(view) { return findChild(view, currentPrefix(view) + "AdvanceButton") }
    function storageStep(view) { return findChild(view, "onboardingStorageStep") }

    function test_welcome_is_centred_with_the_logo_on_top() {
        const view = makeView()
        const column = findChild(view, "onboardingWelcomeColumn")
        const centre = column.mapToItem(view, column.width / 2, column.height / 2)

        fuzzyCompare(centre.x, view.width / 2, 1)
        fuzzyCompare(centre.y, view.height / 2, 1)
        const logo = findChild(view, "onboardingLogo")
        verify(logo.mapToItem(view, 0, 0).y < findChild(view, "onboardingGreeting").mapToItem(view, 0, 0).y)
    }

    function test_github_only_finishes_without_a_storage_step() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        const finished = spy(view, "finished")

        click(view, "onboardingCustomSetupButton")
        click(view, "onboardingStorageCard")
        advance(view)

        compare(chosen.count, 1)
        compare(chosen.signalArguments[0][0], "http")
        compare(finished.count, 1)
    }

    function test_unticking_both_blocks_continue_and_says_why() {
        const view = makeView()

        click(view, "onboardingCustomSetupButton")
        click(view, "onboardingStorageCard")
        click(view, "onboardingGithubCard")

        verify(!advanceButton(view).enabled)
        const hint = findChild(view, "onboardingDownloadsAdvanceHint")
        verify(hint.visible)
        compare(hint.text, "Pick at least one to continue")
    }

    // One page at a time: the welcome screen gives way to the step, and Back
    // brings it back.
    function test_one_page_shows_at_a_time() {
        const view = makeView()
        verify(findChild(view, "onboardingWelcome").visible)
        verify(!findChild(view, "onboardingDownloadsStep").visible)

        // StackLayout swaps which child is visible on its next layout pass.
        click(view, "onboardingCustomSetupButton")
        tryVerify(function() { return findChild(view, "onboardingDownloadsStep").visible })
        verify(!findChild(view, "onboardingWelcome").visible)
        verify(!findChild(view, "onboardingStorageStep").visible)

        click(view, "onboardingDownloadsBackButton")
        compare(findChild(view, "onboardingPages").currentIndex, 0)
        tryVerify(function() { return findChild(view, "onboardingWelcome").visible })
        verify(!findChild(view, "onboardingDownloadsStep").visible)
    }

    function test_storage_only_chooses_logos() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")

        click(view, "onboardingCustomSetupButton")
        click(view, "onboardingGithubCard")
        advance(view)

        compare(chosen.signalArguments[0][0], "logos")
        verify(storageStep(view).visible)
    }

    // Storage only: never switch to GitHub behind the user's back.
    function test_a_failure_with_storage_only_never_falls_back() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        const retry = spy(view, "retryStorageRequested")
        click(view, "onboardingCustomSetupButton")
        click(view, "onboardingGithubCard")
        advance(view)

        verify(!findChild(view, "onboardingStorageSecondaryButton").visible,
               "no Retry while it is still starting")
        view.storageError = "the storage module refused the start command"
        view.storageState = "failed"

        compare(chosen.count, 1)
        compare(chosen.signalArguments[0][0], "logos")
        verify(findChild(view, "onboardingStorageFailed").shown)
        compare(findChild(view, "onboardingStorageFailedReason").text,
                "the storage module refused the start command")
        verify(findChild(view, "onboardingStorageFailedReason").copyButtonItem.visible)
        compare(advanceButton(view).text, "Continue without downloads")

        // Retry sits in the footer beside the way out, not in the notice.
        const retryButton = findChild(view, "onboardingStorageSecondaryButton")
        compare(retryButton.text, "Retry")
        click(view, "onboardingStorageSecondaryButton")
        compare(retry.count, 1)
        verify(!findChild(view, "onboardingStorageFailed").shown, "a retry starts over")
        verify(!retryButton.visible)
    }

    // "Let's go" has no steps: both sources, straight into Basecamp.
    function test_quick_setup_chooses_both_and_finishes_at_once() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        const finished = spy(view, "finished")

        click(view, "onboardingQuickSetupButton")

        compare(chosen.count, 1)
        compare(chosen.signalArguments[0][0], "any")
        compare(finished.count, 1)
        verify(!storageStep(view).visible)
    }

    function chooseBoth(view) {
        click(view, "onboardingCustomSetupButton")
        advance(view)
    }

    function test_both_shows_storage_starting_and_lets_the_user_in_once_running() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        const finished = spy(view, "finished")
        chooseBoth(view)

        compare(chosen.signalArguments[0][0], "any")
        verify(storageStep(view).visible)
        verify(!advanceButton(view).enabled, "nothing to continue to while it starts")

        view.storageState = "running"
        verify(advanceButton(view).enabled)
        advance(view)
        compare(finished.count, 1)
    }

    // Both chosen: a failure keeps both, so a later start brings Storage back.
    function test_a_failure_with_both_keeps_both_and_lets_the_user_in() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        chooseBoth(view)

        view.storageState = "failed"
        view.storageError = "the storage module refused the start command"

        compare(chosen.count, 1, "the source is not changed after the failure")
        verify(findChild(view, "onboardingStorageGithubMeanwhile").shown)
        compare(findChild(view, "onboardingStorageMeanwhileReason").text,
                "the storage module refused the start command")
        verify(findChild(view, "onboardingStorageSecondaryButton").visible)
        verify(advanceButton(view).enabled)
        compare(advanceButton(view).text, "Start Basecamp")
    }

    // A long reason wraps inside the notice instead of running off its edge.
    function test_a_long_failure_reason_wraps_inside_the_notice() {
        const view = makeView()
        click(view, "onboardingCustomSetupButton")
        click(view, "onboardingGithubCard")
        advance(view)

        view.storageError = "storage_module: start() failed: ".repeat(12)
        view.storageState = "failed"
        waitForRendering(view)

        const notice = findChild(view, "onboardingStorageFailed")
        const reason = findChild(view, "onboardingStorageFailedReason")
        verify(reason.width <= notice.messageItem.width,
               "reason " + reason.width + " wider than the message " + notice.messageItem.width)
        verify(reason.height > reason.textItem.font.pixelSize * 2, "it wraps onto more lines")
    }

    // One status line changes in place; a failure swaps it for the notice.
    function test_the_status_line_goes_from_starting_to_running_or_failed() {
        const view = makeView()
        chooseBoth(view)
        const step = storageStep(view)
        const line = findChild(view, "onboardingStorageStatus")
        const text = findChild(view, "onboardingStorageStatusText")

        view.storageState = "starting"
        compare(step.status, "starting")
        verify(line.visible)
        compare(text.text, "Starting Logos Storage…")
        verify(findChild(view, "onboardingStorageSpinner").visible)
        verify(findChild(view, "onboardingStorageElapsed").visible)

        view.storageState = "running"
        compare(step.status, "running")
        compare(text.text, "Logos Storage is running")
        verify(findChild(view, "onboardingStorageRunningIcon").visible)
        verify(!findChild(view, "onboardingStorageSpinner").visible)
        verify(!findChild(view, "onboardingStorageElapsed").visible)

        const other = makeView()
        chooseBoth(other)
        other.storageState = "starting"
        other.storageState = "failed"
        compare(storageStep(other).status, "failed")
        verify(!findChild(other, "onboardingStorageStatus").visible)
        verify(findChild(other, "onboardingStorageGithubMeanwhile").shown)
    }

    // The silent hang this flow exists for: no answer at all counts as failed.
    function test_no_answer_within_the_timeout_counts_as_failed() {
        const view = makeView()
        storageStep(view).timeoutMs = 1000
        chooseBoth(view)

        view.storageState = "waiting"

        tryVerify(function() { return advanceButton(view).enabled }, 5000)
        verify(findChild(view, "onboardingStorageGithubMeanwhile").shown)
    }

    // Nothing is final until the last step: Back works from the storage step
    // and choosing again changes the source.
    function test_back_from_the_storage_step_and_choose_again() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        const finished = spy(view, "finished")
        chooseBoth(view)
        compare(chosen.signalArguments[0][0], "any")

        verify(findChild(view, "onboardingStorageBackButton").visible)
        click(view, "onboardingStorageBackButton")
        tryVerify(function() { return findChild(view, "onboardingDownloadsStep").visible })
        compare(finished.count, 0, "going back does not finish onboarding")

        click(view, "onboardingStorageCard")
        advance(view)

        compare(chosen.count, 2)
        compare(chosen.signalArguments[1][0], "http")
        compare(finished.count, 1)
    }

    function test_back_from_the_downloads_step_reaches_the_welcome_screen() {
        const view = makeView()
        click(view, "onboardingCustomSetupButton")
        click(view, "onboardingDownloadsBackButton")
        tryVerify(function() { return findChild(view, "onboardingWelcome").visible })
    }

    // A failure already reported still shows on coming back, instead of a
    // spinner waiting out the timeout.
    function test_coming_back_to_a_failed_start_shows_the_failure() {
        const view = makeView()
        click(view, "onboardingCustomSetupButton")
        click(view, "onboardingGithubCard")
        advance(view)
        view.storageState = "failed"
        verify(findChild(view, "onboardingStorageFailed").shown)

        click(view, "onboardingStorageBackButton")
        tryVerify(function() { return findChild(view, "onboardingDownloadsStep").visible })
        advance(view)

        tryVerify(function() { return storageStep(view).visible })
        compare(storageStep(view).status, "failed")
        verify(findChild(view, "onboardingStorageFailed").shown)
    }

    // Back and forth does not restart a start in progress: the source is not
    // sent again and the clock keeps running.
    function test_back_and_forth_keeps_the_start_in_progress() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        const retry = spy(view, "retryStorageRequested")
        chooseBoth(view)
        view.storageState = "starting"
        const elapsed = findChild(view, "onboardingStorageElapsed")
        tryVerify(function() { return elapsed.text !== "0:00" }, 3000)

        click(view, "onboardingStorageBackButton")
        tryVerify(function() { return findChild(view, "onboardingDownloadsStep").visible })
        advance(view)
        tryVerify(function() { return storageStep(view).visible })

        compare(chosen.count, 1, "the same source is not sent again")
        compare(retry.count, 0)
        compare(storageStep(view).status, "starting")
        verify(elapsed.text !== "0:00", "the clock carried on, it is " + elapsed.text)
    }
}
