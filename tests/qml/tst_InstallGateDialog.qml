import QtQuick
import QtTest

// Directory import, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"
import Basecamp.AppManager
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

    Component { id: appDialogComp; AddApplicationDialog {} }
    SignalSpy { id: installSpy; signalName: "installRequested" }
    SignalSpy { id: previewSpy; signalName: "versionChangeRequested" }
    SignalSpy { id: gatePreviewSpy; signalName: "optionalPreviewRequested" }

    function selectableOptional() {
        return { name: "storage_module", displayName: "Storage Module", optional: true,
            description: "Stores delivery messages", version: "0.3.0", action: "install",
            versions: [
                {manifest: {version: "0.3.0"}, rootHash: "new-artifact", action: "install"},
                {manifest: {version: "0.2.0"}, rootHash: "old-artifact", action: "upgrade"}
            ] }
    }

    // The install executes the last preview, so the preview request is what carries the choice.
    function test_app_optional_version_pick_is_previewed_separately_from_required_pins() {
        var dlg = appDialogComp.createObject(testCase)
        installSpy.target = dlg
        installSpy.clear()
        dlg.openWith({name: "chat", selectedVersion: "0.3.0",
            optionalPackages: [selectableOptional()]})
        tryVerify(function() { return !!findChild(dlg.contentItem, "packageRow.version.storage_module") })
        var combo = findChild(dlg.contentItem, "packageRow.version.storage_module")
        compare(combo.displayText, "v.0.3.0")
        compare(combo.count, 2)
        previewSpy.target = dlg
        previewSpy.clear()
        combo.activated(1)
        compare(combo.displayText, "v.0.2.0")
        compare(previewSpy.count, 1)
        compare(previewSpy.signalArguments[0][4].storage_module, "0.2.0")
        verify(previewSpy.signalArguments[0][2].storage_module === undefined)
        compare(findChild(dlg.contentItem, "addApplicationDialog.primaryButton").enabled, false)
        dlg.metadata = Object.assign({}, dlg.metadata, {resolutionPending: false})
        mouseClick(findChild(dlg.contentItem, "addApplicationDialog.primaryButton"))
        compare(installSpy.count, 1)
        compare(installSpy.signalArguments[0][0], "chat")
        waitForRendering(testCase)
        var checkbox = findChild(dlg.contentItem, "addApplicationDialog.optional.storage_module")
        mouseClick(checkbox)
        compare(checkbox.checked, false)
        compare(previewSpy.signalArguments[1][3].storage_module, false)
        dlg.destroy()
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

    function test_app_reresolution_drops_optional_versions_that_no_longer_satisfy_constraints() {
        var dlg = appDialogComp.createObject(testCase)
        installSpy.target = dlg
        installSpy.clear()
        dlg.openWith({name: "chat", selectedVersion: "0.3.0",
            optionalPackages: [selectableOptional()]})
        tryVerify(function() { return !!findChild(dlg.contentItem, "packageRow.version.storage_module") })
        findChild(dlg.contentItem, "packageRow.version.storage_module").activated(1)
        var updated = selectableOptional()
        updated.versions = [updated.versions[0]]
        dlg.metadata = {name: "chat", selectedVersion: "0.3.0", optionalPackages: [updated]}
        tryCompare(findChild(dlg.contentItem, "packageRow.version.storage_module"), "displayText", "v.0.3.0")
        previewSpy.target = dlg
        previewSpy.clear()
        dlg.refreshPreview()
        compare(previewSpy.count, 1)
        verify(previewSpy.signalArguments[0][4].storage_module === undefined)
        dlg.destroy()
    }

    function test_app_preview_selects_only_checked_available_optionals() {
        var dlg = appDialogComp.createObject(testCase)
        verify(dlg)
        installSpy.target = dlg
        installSpy.clear()
        var metadata = { name: "chat", repositoryUrl: "https://repo/", selectedVersion: "0.3.0",
            optionalPackages: [
                { name: "storage_module", version: "0.3.0" },
                { name: "missing_module", error: "no candidate" }
            ] }
        dlg.openWith(metadata)
        var primary = findChild(dlg.contentItem, "addApplicationDialog.primaryButton")
        dlg.metadata = Object.assign({}, metadata, {resolutionPending: true})
        compare(primary.enabled, false)
        dlg.metadata = metadata
        compare(primary.enabled, true)
        tryVerify(function() { return !!findChild(dlg.contentItem, "addApplicationDialog.optional.missing_module") })
        var available = findChild(dlg.contentItem, "addApplicationDialog.optional.storage_module")
        var unavailable = findChild(dlg.contentItem, "addApplicationDialog.optional.missing_module")
        verify(available)
        verify(unavailable)
        compare(available.checked, true)
        compare(unavailable.checked, false)
        compare(unavailable.enabled, false)
        previewSpy.target = dlg
        previewSpy.clear()
        mouseClick(available)
        compare(primary.enabled, false)
        compare(previewSpy.count, 1)
        compare(previewSpy.signalArguments[0][3].storage_module, false)
        verify(previewSpy.signalArguments[0][3].missing_module === undefined)
        dlg.metadata = metadata
        mouseClick(findChild(dlg.contentItem, "addApplicationDialog.primaryButton"))
        compare(installSpy.count, 1)
        dlg.destroy()
    }

    function test_package_that_comes_only_with_an_optional_has_no_checkbox() {
        var dlg = appDialogComp.createObject(testCase)
        dlg.openWith({ name: "chat", repositoryUrl: "https://repo/", optionalPackages: [
            { name: "rln", version: "0.10.0", action: "install" },
            { name: "lez_rln", toVersion: "4.2.1", action: "install", optional: true,
              requiredFor: ["rln"], description: "Required by RLN Module" }] })
        tryVerify(function() { return !!findChild(dlg.contentItem, "addApplicationDialog.optional.rln") })
        verify(findChild(dlg.contentItem, "addApplicationDialog.optional.rln").visible)
        var child = findChild(dlg.contentItem, "addApplicationDialog.optional.lez_rln")
        verify(!child || !child.visible)
        dlg.destroy()
    }

    function test_installed_app_lists_optionals_and_installs_pending_changes() {
        var dlg = appDialogComp.createObject(testCase)
        var installed = { name: "storage_module", version: "0.2.0", installedVersion: "0.2.0",
            action: "installed", selected: true }
        var notInstalled = { name: "rln", version: "1.0.0", action: "install", selected: false }
        var metadata = { name: "chat", repositoryUrl: "https://repo/", installStatus: InstallStatus.Installed,
            isInstalled: true, optionalPackages: [installed, notInstalled] }
        dlg.openWith(metadata)
        tryVerify(function() { return !!findChild(dlg.contentItem, "addApplicationDialog.optional.rln") })
        verify(findChild(dlg.contentItem, "addApplicationDialog.optionalPackages").visible)
        compare(findChild(dlg.contentItem, "addApplicationDialog.optional.storage_module").checked, true)
        compare(findChild(dlg.contentItem, "addApplicationDialog.optional.rln").checked, false)
        var primary = findChild(dlg.contentItem, "addApplicationDialog.primaryButton")
        compare(primary.text, "Launch")
        dlg.metadata = Object.assign({}, metadata, {optionalChangesPending: true})
        compare(primary.text, "Install")
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
        var button = findChild(dlg.contentItem, "confirmationDialog.installGate.confirm")
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
}
