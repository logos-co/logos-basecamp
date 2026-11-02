import QtQuick
import QtTest

// Directory import: see tst_IntentChooserDialog.qml.
import "../../src/Basecamp/Shell"

// First-run onboarding, driven through its own buttons. What matters is which
// download source each route reports. Onboarding only saves it: the storage
// node starts later, for the first download that needs it.
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

    function advance(view) { click(view, "onboardingDownloadsAdvanceButton") }
    function advanceButton(view) { return findChild(view, "onboardingDownloadsAdvanceButton") }

    function test_welcome_is_centred_with_the_logo_on_top() {
        const view = makeView()
        const column = findChild(view, "onboardingWelcomeColumn")
        const centre = column.mapToItem(view, column.width / 2, column.height / 2)

        fuzzyCompare(centre.x, view.width / 2, 1)
        fuzzyCompare(centre.y, view.height / 2, 1)
        const logo = findChild(view, "onboardingLogo")
        verify(logo.mapToItem(view, 0, 0).y < findChild(view, "onboardingGreeting").mapToItem(view, 0, 0).y)
    }

    function test_github_only_chooses_http_and_finishes() {
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

        click(view, "onboardingDownloadsBackButton")
        compare(findChild(view, "onboardingPages").currentIndex, 0)
        tryVerify(function() { return findChild(view, "onboardingWelcome").visible })
        verify(!findChild(view, "onboardingDownloadsStep").visible)
    }

    // No step waits for the node: Storage only finishes at once too.
    function test_storage_only_chooses_logos_and_finishes() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        const finished = spy(view, "finished")

        click(view, "onboardingCustomSetupButton")
        click(view, "onboardingGithubCard")
        compare(advanceButton(view).text, "Start Basecamp")
        advance(view)

        compare(chosen.count, 1)
        compare(chosen.signalArguments[0][0], "logos")
        compare(finished.count, 1)
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
    }

    function test_both_chooses_any_and_finishes() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        const finished = spy(view, "finished")

        click(view, "onboardingCustomSetupButton")
        compare(advanceButton(view).text, "Start Basecamp")
        advance(view)

        compare(chosen.count, 1)
        compare(chosen.signalArguments[0][0], "any")
        compare(finished.count, 1)
    }

    function test_back_from_the_downloads_step_reaches_the_welcome_screen() {
        const view = makeView()
        const chosen = spy(view, "sourceChosen")
        click(view, "onboardingCustomSetupButton")
        click(view, "onboardingDownloadsBackButton")
        tryVerify(function() { return findChild(view, "onboardingWelcome").visible })
        compare(chosen.count, 0, "going back chooses nothing")
    }
}
