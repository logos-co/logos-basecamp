import QtQuick
import QtTest

// Directory import, not `import Basecamp.Settings` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Settings"

TestCase {
    name: "RepositoryContract"
    when: windowShown
    // A TestCase starts hidden; the view takes clicks only if this shows.
    visible: true
    width: 900
    height: 700

    // A row's switch and trash button share its top line. The trash button
    // removes a repository at once, but asks first for the default one.
    function test_a_rows_trash_button() {
        var view = createTemporaryObject(viewComp, testCase, { repositories: [
            { url: "https://default/", displayName: "Logos", isDefault: true, enabled: true },
            { url: "https://added/", displayName: "Added", description: "Someone's packages", enabled: false }] })
        removeSpy.target = view
        removeSpy.clear()
        tryVerify(function() { return !!findChild(view, "repositories.remove.https://added/") })
        waitForRendering(view)

        var toggle = findChild(view, "repositories.enableSwitch.https://added/")
        var trash = findChild(view, "repositories.remove.https://added/")
        compare(trash.parent, toggle.parent, "on the switch's line")
        verify(trash.x > toggle.x, "after the switch")
        mouseClick(trash)
        compare(removeSpy.count, 1)
        compare(removeSpy.signalArguments[0][0], "https://added/")

        mouseClick(findChild(view, "repositories.remove.https://default/"))
        var cancel = findChild(view, "repositories.removeConfirm.cancel")
        tryVerify(function() { return cancel.visible })
        compare(removeSpy.count, 1, "the default is not removed without asking")
        // Until laid out, the dialog's buttons sit on top of each other.
        waitForRendering(testCase)
        mouseClick(cancel)
        compare(removeSpy.count, 1)
    }

    // A read that failed leaves the rows shown and says why; the next read
    // that works clears it, but not a change the downloader refused.
    function test_a_failed_read_keeps_the_rows_and_says_why() {
        var view = createTemporaryObject(viewComp, testCase, { repositories: [
            { url: "https://added/", displayName: "Added", enabled: true }] })
        var banner = findChild(view, "repositories.errorBanner")
        var why = "Couldn't read the repositories: package_downloader did not answer within 30 s."

        view.reportOperationResult("list", "", false, why)
        compare(banner.message, why)
        verify(banner.shown)
        verify(!!findChild(view, "repositories.remove.https://added/"), "the row stays")

        view.reportOperationResult("list", "", true, "")
        compare(banner.message, "")

        view.reportOperationResult("add", "https://bad/", false, "Not a repository")
        view.reportOperationResult("list", "", true, "")
        compare(banner.message, "Not a repository")
    }

    Component { id: viewComp; RepositoriesView { width: 760; height: 600 } }
    SignalSpy { id: removeSpy; signalName: "removeRequested" }

    property var testCase: this
}
