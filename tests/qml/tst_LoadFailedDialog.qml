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
