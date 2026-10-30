import QtQuick
import QtTest

// Directory import, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"
import Basecamp.AppManager
import Basecamp.Backend 1.0

// The loadFailed mode is the only feedback a user gets when a UI app's view
// fails to load. Regression guard for the "spinner, then nothing" bug: the
// dialog must name the app, carry the raw error text (multi-line QML compile
// errors, selectable for bug reports) and offer exactly one way out.
TestCase {
    id: testCase
    name: "LoadFailedDialog"
    when: windowShown
    width: 900
    height: 900

    Component {
        id: dialogComp
        ConfirmationDialog {
            objectName: "confirmationDialog.loadFailed"
            mode: "loadFailed"
            displayNameLookup: function(name) {
                return name === "blockchain_ui" ? "Blockchain" : "";
            }
        }
    }

    readonly property string compileError:
        "file:///plugins/blockchain_ui/qml/BlockchainView.qml:521:9: Type OnboardingFlow unavailable\n"
        + "file:///plugins/blockchain_ui/qml/views/LeaderRewardsView.qml:193:9: LogosStatCard is not a type\n"

    function test_shows_display_name_and_full_error_text() {
        var dlg = dialogComp.createObject(testCase)
        dlg.openWithLoadError("blockchain_ui", compileError)
        tryVerify(function() { return dlg.opened })

        compare(dlg.mode, "loadFailed")
        compare(dlg.moduleName, "blockchain_ui")
        verify(findChildWithText(dlg.contentItem, "Couldn't Load App"), "title rendered")
        verify(findChildWithText(dlg.contentItem, "'Blockchain' failed to load. Details:"),
               "body names the app by display name")

        var err = findChild(dlg.contentItem, "confirmationDialog.loadFailed.error")
        verify(err, "error area present")
        verify(err.visible, "error area visible in loadFailed mode")
        // trimmed, otherwise verbatim — both lines survive
        compare(err.text, compileError.trim())
        verify(err.readOnly, "error text is read-only")
        verify(err.selectByMouse, "error text is selectable")
        dlg.destroy()
    }

    // A startup notice is about a module, not an app: it brings its own title,
    // and an app failure after it gets the app title back.
    function test_a_startup_notice_carries_its_own_title() {
        var dlg = dialogComp.createObject(testCase)
        dlg.openWithLoadError("capability_module", "Basecamp started without the modules you installed.",
                              "Installed Modules Not Used")
        tryVerify(function() { return dlg.opened })
        verify(findChildWithText(dlg.contentItem, "Installed Modules Not Used"), "notice title rendered")
        verify(!findChildWithText(dlg.contentItem, "Couldn't Load App"), "app title replaced")
        dlg.close()
        tryVerify(function() { return !dlg.visible })

        dlg.openWithLoadError("blockchain_ui", compileError)
        tryVerify(function() { return dlg.opened })
        verify(findChildWithText(dlg.contentItem, "Couldn't Load App"), "app title is back")
        dlg.destroy()
    }

    function test_falls_back_to_module_name_and_placeholder_error() {
        var dlg = dialogComp.createObject(testCase)
        dlg.openWithLoadError("unknown_ui", "")
        tryVerify(function() { return dlg.opened })
        verify(findChildWithText(dlg.contentItem, "'unknown_ui' failed to load. Details:"))
        compare(findChild(dlg.contentItem, "confirmationDialog.loadFailed.error").text,
                "No error details were reported.")
        dlg.destroy()
    }

    function test_single_ok_button_closes() {
        var dlg = dialogComp.createObject(testCase)
        dlg.openWithLoadError("blockchain_ui", compileError)
        tryVerify(function() { return dlg.opened })

        var cancel = findChild(dlg.contentItem, "confirmationDialog.loadFailed.cancel")
        verify(cancel)
        verify(!cancel.visible, "no Cancel in an informational dialog")
        var ok = findChild(dlg.contentItem, "confirmationDialog.loadFailed.confirm")
        verify(ok)
        compare(ok.text, "OK")
        mouseClick(ok)
        tryVerify(function() { return !dlg.visible })
        dlg.destroy()
    }

    readonly property string damagedMessage:
        "Its files have been modified or damaged since it was installed, so it was not "
        + "started. Reinstall or uninstall it in Package Manager.\n\n"
        + "installed module has been modified or damaged since install: installed content "
        + "does not match manifest hashes"

    // Words are not broken across lines: "Reinstall" used to render as
    // "Re" / "install" because the area wrapped at any character. Where a
    // break lands depends on the width, so this checks a spread of them.
    function test_prose_wraps_between_words() {
        var dlg = dialogComp.createObject(testCase)
        dlg.openWithLoadError("blockchain_ui", damagedMessage)
        tryVerify(function() { return dlg.opened })
        var err = findChild(dlg.contentItem, "confirmationDialog.loadFailed.error")
        verify(err)

        for (var w = 300; w <= 600; w += 7) {
            err.width = w
            verify(err.lineCount > 1, "the message wraps at width " + w)
            var re = /\S+/g
            var m
            while ((m = re.exec(err.text)) !== null) {
                var first = err.positionToRectangle(m.index).y
                var last = err.positionToRectangle(m.index + m[0].length - 1).y
                compare(last, first, "at width " + w + ", '" + m[0] + "' is split across lines")
            }
        }
        dlg.destroy()
    }

    // ...while a token longer than a line still wraps instead of overflowing.
    function test_a_token_longer_than_a_line_still_wraps() {
        var path = "file:///plugins/blockchain_ui/" + new Array(40).join("very_long_directory_name/")
                 + "BlockchainView.qml:521:9: Type OnboardingFlow unavailable"
        var dlg = dialogComp.createObject(testCase)
        dlg.openWithLoadError("blockchain_ui", path)
        tryVerify(function() { return dlg.opened })
        var err = findChild(dlg.contentItem, "confirmationDialog.loadFailed.error")
        tryVerify(function() { return err.lineCount > 1 }, 5000, "the path wraps")
        verify(err.contentWidth <= err.width + 1,
               "content " + err.contentWidth + " fits the area " + err.width)
        dlg.destroy()
    }

    // Depth-first search for a visible Text-like item whose text matches.
    function findChildWithText(item, text) {
        if (!item) return null
        if (item.text !== undefined && item.text === text && item.visible) return item
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) {
            var hit = findChildWithText(kids[i], text)
            if (hit) return hit
        }
        return null
    }
}
