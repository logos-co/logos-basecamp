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
}
