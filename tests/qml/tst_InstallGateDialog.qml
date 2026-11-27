import QtQuick
import QtTest

// Directory import, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"
import Basecamp.Backend 1.0

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
    width: 900
    height: 1100

    Component {
        id: dialogComp
        ConfirmationDialog {}
    }

    SignalSpy { id: gatePreviewSpy; signalName: "optionalPreviewRequested" }
    SignalSpy { id: gateCancelSpy; signalName: "cancelClicked" }

    // Its request ended: only the dialog showing it closes, and it answers
    // nothing. A cancel goes by package name, so it would have cancelled
    // whatever asked about the same package next.
    function test_a_withdrawn_request_closes_without_an_answer() {
        var dlg = dialogComp.createObject(testCase)
        gateCancelSpy.target = dlg
        gateCancelSpy.clear()
        dlg.openWithInstallGate("chat", "1.0.0", [], "package_manager_ui", true, true, "a1")
        tryCompare(dlg, "opened", true)
        verify(!dlg.withdraw("b1"))
        verify(!dlg.withdraw(""))
        verify(dlg.visible)
        verify(dlg.withdraw("a1"))
        tryCompare(dlg, "visible", false)
        compare(gateCancelSpy.count, 0)
        dlg.destroy()
    }

    // The same package asked again the moment the first request ended: the
    // retry is what shows, and nothing cancels it.
    function test_a_retry_right_after_a_withdrawal_is_not_cancelled() {
        var dlg = dialogComp.createObject(testCase)
        gateCancelSpy.target = dlg
        gateCancelSpy.clear()
        dlg.openWithInstallGate("chat", "1.0.0", [], "package_manager_ui", true, true, "a1")
        tryCompare(dlg, "opened", true)
        dlg.withdraw("a1")
        dlg.openWithInstallGate("chat", "1.1.0", [], "package_manager_ui", true, true, "b1")
        wait(500)
        verify(dlg.visible)
        compare(dlg.requestId, "b1")
        compare(dlg.upgradeTargetVersion, "1.1.0")
        compare(gateCancelSpy.count, 0)
        // Escape still answers the request on screen.
        keyClick(Qt.Key_Escape)
        tryCompare(dlg, "visible", false)
        compare(gateCancelSpy.count, 1)
        compare(gateCancelSpy.signalArguments[0][0], "chat")
        dlg.destroy()
    }

    function selectableOptional() {
        return { name: "storage_module", displayName: "Storage Module", optional: true,
            description: "Stores delivery messages", version: "0.3.0", action: "install",
            versions: [
                {manifest: {version: "0.3.0"}, rootHash: "new-artifact", action: "install"},
                {manifest: {version: "0.2.0"}, rootHash: "old-artifact", action: "upgrade"}
            ] }
    }

    function test_gate_optional_version_picker_uses_resolved_version_and_resets() {
        var dlg = dialogComp.createObject(testCase)
        dlg.openWithInstallGate("delivery_module", "0.3.0", [selectableOptional()], "", true)
        tryVerify(function() { return !!findChild(dlg.contentItem, "packageRow.version.storage_module") })
        var combo = findChild(dlg.contentItem, "packageRow.version.storage_module")
        compare(combo.displayText, "v.0.3.0")
        combo.activated(1)
        compare(combo.displayText, "v.0.2.0")
        compare(dlg.optionalPickedVersions.storage_module, "0.2.0")
        dlg.close()
        dlg.openWithInstallGate("delivery_module", "0.3.0", [selectableOptional()], "", true)
        compare(dlg.optionalPickedVersions.storage_module, undefined)
        dlg.destroy()
    }

    // The requester's pick for an optional is what the dialog shows and
    // answers with: it showed the newest while the plan held the pick.
    function test_an_optional_starts_at_the_version_the_requester_picked() {
        var dlg = dialogComp.createObject(testCase)
        var offer = selectableOptional()
        offer.requestedVersion = "0.2.0"
        dlg.openWithInstallGate("delivery_module", "0.3.0", [offer], "app_manager_ui", true)
        tryVerify(function() { return !!findChild(dlg.contentItem, "packageRow.version.storage_module") })
        compare(findChild(dlg.contentItem, "packageRow.version.storage_module").displayText, "v.0.2.0")
        compare(dlg.optionalPickedVersions.storage_module, "0.2.0")
        dlg.close()
        // A version it doesn't have, or the source can't serve, is no pick.
        offer.requestedVersion = "9.9.9"
        dlg.openWithInstallGate("delivery_module", "0.3.0", [offer], "app_manager_ui", true)
        compare(dlg.optionalPickedVersions.storage_module, undefined)
        tryCompare(findChild(dlg.contentItem, "packageRow.version.storage_module"), "displayText", "v.0.3.0")
        dlg.close()
        offer.requestedVersion = "0.2.0"
        offer.versions[1].sourceAvailable = false
        dlg.openWithInstallGate("delivery_module", "0.3.0", [offer], "app_manager_ui", true)
        compare(dlg.optionalPickedVersions.storage_module, undefined)
        dlg.destroy()
    }

    // What the install stops shows before it is approved: what runs on a
    // package it replaces, for the rows that install. The gate used to say
    // nothing, and the package was replaced under them.
    function test_the_gate_names_what_replacing_a_package_stops() {
        var dlg = dialogComp.createObject(testCase)
        const upgrade = { name: "signer", displayName: "Signer", action: "upgrade",
                          fromVersion: "1.0.0", toVersion: "2.0.0", closes: ["wallet_ui"] }
        var offer = selectableOptional()
        offer.action = "upgrade"
        offer.closes = ["chat_ui", "wallet_ui"]
        offer.selected = false
        dlg.openWithInstallGate("delivery_module", "0.3.0", [upgrade, offer], "app_manager_ui", true)
        tryVerify(function() { return !!findChild(dlg.contentItem, "confirmationDialog.running") })
        const running = findChild(dlg.contentItem, "confirmationDialog.running")
        tryCompare(running, "count", 1)
        verify(running.visible)
        dlg.optionalSelection = ({ storage_module: true })
        tryCompare(running, "count", 2)
        dlg.destroy()
    }

    function test_package_that_comes_only_with_an_optional_has_no_checkbox() {
        var dlg = dialogComp.createObject(testCase)
        dlg.openWithInstallGate("chat", "1.0.0", [
            { name: "rln", optional: true, version: "0.10.0", action: "install" },
            { name: "lez_rln", toVersion: "4.2.1", action: "install", optional: true,
              requiredFor: ["rln"], description: "Required by RLN Module" }], "", true)
        tryVerify(function() { return !!findChild(dlg.contentItem, "confirmationDialog.optional.rln") })
        verify(findChild(dlg.contentItem, "confirmationDialog.optional.rln").visible)
        var child = findChild(dlg.contentItem, "confirmationDialog.optional.lez_rln")
        verify(!child || !child.visible)
        dlg.destroy()
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
    function test_available_optionals_default_checked_and_unavailable_disabled() {
        var dlg = dialogComp.createObject(testCase)
        verify(dlg)
        dlg.openWithInstallGate("delivery_module", "0.3.0", [
            { name: "storage_module", optional: true, version: "0.3.0" },
            { name: "missing_module", optional: true, error: "no candidate" }
        ], "package_manager_ui", false)
        compare(dlg.selectedOptionalNames().length, 1)
        compare(dlg.selectedOptionalNames()[0], "storage_module")
        tryVerify(function() { return !!findChild(dlg.contentItem, "confirmationDialog.optional.missing_module") })
        var available = findChild(dlg.contentItem, "confirmationDialog.optional.storage_module")
        var unavailable = findChild(dlg.contentItem, "confirmationDialog.optional.missing_module")
        verify(available)
        verify(unavailable)
        compare(available.checked, true)
        compare(unavailable.checked, false)
        compare(unavailable.enabled, false)
        mouseClick(available)
        compare(dlg.selectedOptionalNames().length, 0)
        compare(dlg.mandatoryChanges.length, 0)
        dlg.destroy()
    }

    function test_gate_checkbox_reresolves_and_discovered_optionals_default_checked() {
        var dlg = dialogComp.createObject(testCase)
        gatePreviewSpy.target = dlg
        gatePreviewSpy.clear()
        var rln = {name: "rln", optional: true, version: "0.10.0"}
        dlg.openWithInstallGate("delivery_module", "0.3.0", [rln], "", true)
        tryVerify(function() { return !!findChild(dlg.contentItem, "confirmationDialog.optional.rln") })
        var button = findChild(dlg.footer, "confirmationDialog.installGate.confirm")
        waitForRendering(testCase)
        verify(gatePreviewSpy.valid)
        mouseClick(findChild(dlg.contentItem, "confirmationDialog.optional.rln"))
        compare(gatePreviewSpy.count, 1)
        compare(gatePreviewSpy.signalArguments[0][1].rln, false)
        compare(button.enabled, false)
        dlg.depChanges = [rln, {name: "nested", optional: true, version: "1.0.0"},
            {name: "lez_rln", toVersion: "0.10.0", action: "install"}]
        dlg.resolutionPending = false
        compare(dlg.mandatoryChanges[0].name, "lez_rln")
        compare(dlg.selectedOptionalNames().length, 1)
        compare(dlg.selectedOptionalNames()[0], "nested")
        compare(button.enabled, true)
        dlg.destroy()
    }

    function test_upgrade_optionals_reset_for_each_dialog() {
        var dlg = dialogComp.createObject(testCase)
        verify(dlg)
        dlg.openWithUpgrade("delivery_module", "0.3.0", 0, [], [],
            [{ name: "storage_module", optional: true, version: "0.3.0" }], "", true)
        compare(dlg.selectedOptionalNames()[0], "storage_module")
        dlg.optionalSelection = ({storage_module: false})
        compare(dlg.selectedOptionalNames().length, 0)
        dlg.close()
        dlg.openWithUpgrade("delivery_module", "0.3.0", 0, [], [],
            [{ name: "storage_module", optional: true, version: "0.3.0" }], "", true)
        compare(dlg.selectedOptionalNames()[0], "storage_module")
        dlg.destroy()
    }
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

    function requesterLineOf(dlg) {
        function find(item) {
            if (item.objectName === "confirmationDialog.requester") return item
            for (var i = 0; i < item.children.length; ++i) {
                var hit = find(item.children[i])
                if (hit) return hit
            }
            return null
        }
        return find(dlg.contentItem ? dlg.contentItem : dlg)
    }

    // Both dialogs dropped whether the app that asked ships with Logos, so
    // Package Manager's own requests read as coming from an app that is not
    // part of Logos.
    function test_install_gate_says_whether_the_requester_ships_with_logos() {
        var dlg = dialogComp.createObject(testCase)
        verify(dlg)
        dlg.openWithInstallGate("chat", "1.0.0", [], "package_manager_ui", true)
        var line = requesterLineOf(dlg)
        verify(line, "no confirmationDialog.requester line")
        verify(line.text.indexOf("which ships with Logos") !== -1, line.text)
        dlg.close()
        dlg.openWithInstallGate("chat", "1.0.0", [], "evil_ui", false)
        verify(line.text.indexOf("which is not part of Logos") !== -1, line.text)
        dlg.destroy()
    }

    // App Manager's requests are the person's own clicks there: an app, and no
    // word of who asked. Anyone else's keep both.
    function test_app_manager_asks_in_simpler_words() {
        var dlg = dialogComp.createObject(testCase, { appsApp: "app_manager_ui" })
        verify(dlg)
        dlg.openWithInstallGate("chat", "1.0.0", [], "app_manager_ui", true)
        tryCompare(dlg, "opened", true)
        compare(dlg.title, "Install App?")
        compare(requesterLineOf(dlg).visible, false)
        dlg.close()
        tryCompare(dlg, "visible", false)
        dlg.openWithUpgrade("chat", "2.0.0", 1, [], [], [], "app_manager_ui", true)
        tryCompare(dlg, "opened", true)
        compare(dlg.title, "Downgrade App?")
        dlg.close()
        tryCompare(dlg, "visible", false)
        dlg.openWithInstallGate("chat", "1.0.0", [], "package_manager_ui", true)
        tryCompare(dlg, "opened", true)
        compare(dlg.title, "Install Package?")
        compare(requesterLineOf(dlg).visible, true)
        dlg.destroy()
    }

    function test_upgrade_says_whether_the_requester_ships_with_logos() {
        var dlg = dialogComp.createObject(testCase)
        verify(dlg)
        dlg.openWithUpgrade("chat", "2.0.0", 0, [], [], [], "package_manager_ui", true)
        var line = requesterLineOf(dlg)
        verify(line, "no confirmationDialog.requester line")
        verify(line.text.indexOf("which ships with Logos") !== -1, line.text)
        dlg.close()
        dlg.openWithUpgrade("chat", "2.0.0", 0, [], [], [], "evil_ui", false)
        verify(line.text.indexOf("which is not part of Logos") !== -1, line.text)
        dlg.destroy()
    }
}
