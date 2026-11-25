import QtQuick
import QtTest

// Directory import, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"

// Who asked for a removal: the line names the app, claims nothing the shell
// never checked, and leaves a name that reads like a placeholder as it is.
TestCase {
    id: testCase
    name: "UninstallDialog"
    when: windowShown

    Component {
        id: dialogComp
        UninstallDialog {}
    }

    function requesterLineOf(dlg) {
        function find(item) {
            if (item.objectName === "uninstallDialog.requester") return item
            for (var i = 0; i < item.children.length; ++i) {
                var hit = find(item.children[i])
                if (hit) return hit
            }
            return null
        }
        return find(dlg.contentItem ? dlg.contentItem : dlg)
    }

    function test_requester_line_is_filled_in_one_pass() {
        var dlg = dialogComp.createObject(testCase)
        verify(dlg)
        dlg.openWithPlan({ requester: "evil_ui", requesterDisplayName: "Wallet %2",
                           requesterBundled: false })
        var line = requesterLineOf(dlg)
        verify(line, "no uninstallDialog.requester line")
        compare(line.text, "Requested by Wallet %2 (evil_ui), which is not part of Logos.")
        dlg.close()

        dlg.openWithPlan({ requester: "evil_ui", requesterBundled: false })
        compare(line.text, "Requested by evil_ui, which is not part of Logos.")
        dlg.destroy()
    }

    SignalSpy { id: cancelledSpy; signalName: "cancelled" }
    SignalSpy { id: confirmedSpy; signalName: "confirmed" }

    // Its request ended: only the dialog showing it closes, and it answers
    // nothing. Basecamp's own plan has no request to end.
    function test_a_withdrawn_request_closes_without_an_answer() {
        var dlg = dialogComp.createObject(testCase)
        cancelledSpy.target = dlg
        confirmedSpy.target = dlg
        cancelledSpy.clear()
        confirmedSpy.clear()
        dlg.openWithPlan({ kind: "packages", batch: ["chat"], requestId: "a1" })
        tryCompare(dlg, "opened", true)
        verify(!dlg.withdraw("b1"))
        verify(!dlg.withdraw(""))
        verify(dlg.visible)
        verify(dlg.withdraw("a1"))
        tryCompare(dlg, "visible", false)
        compare(cancelledSpy.count, 0)
        compare(confirmedSpy.count, 0)

        dlg.openWithPlan({ kind: "packages", batch: ["chat"] })
        tryCompare(dlg, "opened", true)
        verify(!dlg.withdraw("a1"))
        verify(dlg.visible)
        dlg.destroy()
    }
}
