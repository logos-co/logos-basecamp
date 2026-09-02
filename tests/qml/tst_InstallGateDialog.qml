import QtQuick
import QtTest

// Directory import, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"

// The install gate is a CONSENT screen: what it lists is what the user is
// agreeing to install. It must never assert more than it knows.
//
// Regression guard. PMUI used to own this dialog and fed it the same
// resolution it then installed, so the two could not disagree. Since the ack
// process moved consent to this host gate (logos-package-manager-ui 76e6435),
// the gate resolves dependencies INDEPENDENTLY of the installer — and when
// that resolution yields nothing, for any reason, the dialog states
// "No other packages need to change." It says that whether the resolver
// returned an empty set or was never consulted at all: a fresh install of a
// package with an uninstalled dependency showed that sentence, then installed
// the dependency anyway.
TestCase {
    id: testCase
    name: "InstallGateDialog"
    when: windowShown

    Component {
        id: dialogComp
        ConfirmationDialog {}
    }

    function bodyTextOf(dlg) {
        // The body paragraph is the only text carrying the claim; find it by
        // content rather than by index so layout changes don't break this.
        var out = []
        function walk(item) {
            if (item.text !== undefined && typeof item.text === "string")
                out.push(item.text)
            for (var i = 0; i < item.children.length; ++i) walk(item.children[i])
        }
        walk(dlg.contentItem ? dlg.contentItem : dlg)
        return out.join("\n")
    }

    // Baseline: a genuinely empty, genuinely resolved set may say so.
    function test_resolved_empty_set_may_claim_nothing_changes() {
        var dlg = dialogComp.createObject(testCase)
        verify(dlg)
        dlg.openWithInstallGate("chat", "1.0.0", [], "package_manager_ui", false)
        var body = bodyTextOf(dlg)
        verify(body.indexOf("Install 'chat'") !== -1)
        dlg.destroy()
    }

    // The regression: the dialog lists dependency changes when it has them.
    function test_resolved_changes_are_listed() {
        var dlg = dialogComp.createObject(testCase)
        verify(dlg)
        dlg.openWithInstallGate("chat", "1.0.0",
            [{ name: "chat_module", action: "install", toVersion: "2.0.0",
               fromVersion: "", repositoryName: "logos" }],
            "package_manager_ui", false)
        var body = bodyTextOf(dlg)
        verify(body.indexOf("chat_module") !== -1,
                "a resolved dependency must be named in the dialog: " + body)
        verify(body.indexOf("No other packages need to change") === -1,
                "must not claim nothing changes while listing a change")
        dlg.destroy()
    }

    // The bug. When the gate could not determine the dependency set, the
    // dialog must not claim there is nothing to change — that sentence is a
    // statement about the world, and an unresolved gate has not looked at it.
    function test_unresolved_set_does_not_claim_nothing_changes() {
        var dlg = dialogComp.createObject(testCase)
        verify(dlg)
        // resolved = false: no repositoryUrl to resolve against, or the
        // resolver call failed. Same empty list, different meaning.
        dlg.openWithInstallGate("chat", "1.0.0", [], "package_manager_ui", false,
                                /*depChangesResolved=*/false)
        var body = bodyTextOf(dlg)
        verify(body.indexOf("No other packages need to change") === -1,
                "unresolved dependencies must not be reported as 'nothing to "
                + "change' — that is the sentence shown while a dependency was "
                + "silently installed. Body was: " + body)
        dlg.destroy()
    }
}
