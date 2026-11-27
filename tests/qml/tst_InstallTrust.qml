import QtQuick
import QtTest
import Logos.Controls
import Logos.Icons
import Logos.Theme

// Directory imports, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"
import "../../src/Basecamp/Settings"
import Basecamp.Backend 1.0
import Basecamp.Common

// Trust at install: what the install and upgrade dialogs say about who signed
// what they would install, and what they let the user do about it: a badge on
// every package's row, and a summary under the rows when not everything comes
// from a trusted publisher. The rules that decide it are
// PackageCoordinator's (tests/install_trust_test.cpp); these feed the dialogs
// the shapes it publishes.
TestCase {
    id: testCase
    name: "InstallTrust"
    when: windowShown
    // A TestCase starts hidden, and the Settings views are plain Items, not
    // popups: they show and take clicks only if this does.
    visible: true
    width: 900
    height: 1100

    readonly property string otherDid: "did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6Im90aGVyIn0"

    Component { id: dialogComp; ConfirmationDialog {} }
    Component { id: lineComp; InstallTrustLine {} }
    Component { id: publishersComp; TrustedPublishersView { width: 760; height: 600 } }

    SignalSpy { id: continueSpy; signalName: "continueClicked" }
    SignalSpy { id: trustContinueSpy; signalName: "trustAndContinueClicked" }
    SignalSpy { id: removeSpy; signalName: "removeRequested" }

    function trusted() {
        return { needsPrompt: false, blocked: false, packages: [], publishers: [], byName: {} }
    }

    function signedByLogos(names) {
        var byName = {}
        for (var i = 0; i < names.length; ++i)
            byName[names[i]] = { status: "trusted", publisher: "Logos" }
        return { needsPrompt: false, blocked: false, packages: [], publishers: [], byName: byName }
    }

    // An app and its module signed by a publisher not trusted yet, and an
    // unsigned dependency.
    function untrusted() {
        return { needsPrompt: true, blocked: false, canTrust: true, trustCoversAll: false,
            packages: [
                { name: "chat", version: "1.0.0", status: "untrusted", did: otherDid, signerName: "Other" },
                { name: "chat_module", version: "1.0.0", status: "untrusted", did: otherDid, signerName: "Other" },
                { name: "rln", version: "0.1.0", status: "unsigned" }],
            publishers: [publisher("Other", ["chat", "chat_module"])],
            trust: publisher("Other", ["chat", "chat_module"]),
            byName: {
                chat: { status: "untrusted", publisher: "Other", impersonates: "" },
                chat_module: { status: "untrusted", publisher: "Other", impersonates: "" },
                rln: { status: "unsigned" } } }
    }

    function publisher(name, packages, impersonates) {
        return { did: otherDid, fingerprint: "AE21 6C2E F524 7A37", name: name,
                 url: "https://other.example", packages: packages, impersonates: impersonates || "" }
    }

    // Signed by a publisher not trusted yet, and nothing else: trusting it
    // would cover the lot.
    function trustable() {
        var t = untrusted()
        t.packages = t.packages.filter(function(p) { return p.status === "untrusted" })
        delete t.byName.rln
        t.trustCoversAll = true
        return t
    }

    function unsignedOnly() {
        return { needsPrompt: true, blocked: false,
            packages: [{ name: "rln", version: "0.1.0", status: "unsigned" }], publishers: [],
            byName: { rln: { status: "unsigned" } } }
    }

    // A damaged package beside an untrusted publisher: no way through at all.
    function blocked() {
        var t = untrusted()
        t.blocked = true
        t.packages.push({ name: "storage_module", version: "0.3.0", status: "invalid" })
        t.byName.storage_module = { status: "invalid" }
        return t
    }

    function change(name) {
        return { name: name, action: "install", toVersion: "1.0.0", fromVersion: "" }
    }

    function shown(parent, name) {
        var item = findChild(parent, name)
        return !!item && item.visible
    }

    // The icon a mark shows, in a word.
    function iconWord(markItem) {
        var source = String(findChild(markItem, "installTrust.markIcon").source)
        return source === String(LogosIcons.check) ? "check"
             : source === String(LogosIcons.warning) ? "warning" : source
    }

    // A row's mark: its icon in a word and, after a bar, what it says on hover.
    function mark(parent, name) {
        var item = findChild(parent, name)
        return item && item.visible ? iconWord(item) + " | " + item.description : ""
    }

    // The notice under the rows: its title, and its message line by line.
    function noticeTitle(parent) {
        var item = findChild(parent, "installTrust.warning")
        return item && item.visible ? item.title : ""
    }
    function noticeLines(parent) {
        var item = findChild(parent, "installTrust.warning")
        return item && item.visible ? item.message.split("\n") : []
    }

    // What package_manager_ui's dialog says about the package it installs.
    function bodyOf(dlg) {
        return findChild(dlg.contentItem, "confirmationDialog.body").text
    }

    function openGate(name, changes) {
        var dlg = createTemporaryObject(dialogComp, testCase)
        dlg.openWithInstallGate(name || "chat", "1.0.0", changes || [], "package_manager_ui", false)
        return dlg
    }

    // ── The install gate (package_manager_ui) ──

    function test_gate_waits_for_the_check() {
        var dlg = openGate()
        var confirm = findChild(dlg.footer, "confirmationDialog.installGate.confirm")
        dlg.trust = { pending: true }
        compare(confirm.enabled, false)
        verify(shown(dlg.contentItem, "installTrust.checking"))
        compare(bodyOf(dlg).indexOf("Install 'chat' v1.0.0."), 0, bodyOf(dlg))

        dlg.trust = trusted()
        compare(confirm.enabled, true)
        compare(confirm.text, "Install")
        verify(!shown(dlg.contentItem, "installTrust.warning"))
        verify(!shown(dlg.footer, "confirmationDialog.installGate.trustAndConfirm"))
    }

    // Nothing to decide still says who signed it. A fully trusted install used
    // to show nothing at all, which reads as "never checked".
    function test_a_trusted_install_says_who_signed_it() {
        var dlg = openGate("blockchain_module")
        dlg.trust = signedByLogos(["blockchain_module"])
        compare(bodyOf(dlg).indexOf("Install 'blockchain_module' v1.0.0, signed by Logos."), 0, bodyOf(dlg))
        verify(!shown(dlg.contentItem, "installTrust.warning"))
        var confirm = findChild(dlg.footer, "confirmationDialog.installGate.confirm")
        compare(confirm.text, "Install")
        compare(confirm.enabled, true)
    }

    // basecamp#333: what the app's repository doesn't provide is said, the
    // installed copy it will use or the repository it comes from.
    function test_says_what_its_repository_doesnt_provide() {
        var dlg = openGate("muster_ui", [change("wallet")])
        var t = signedByLogos(["muster_ui", "wallet"])
        dlg.trust = t
        verify(!shown(dlg.contentItem, "outsideDependencies"))

        t = signedByLogos(["muster_ui", "wallet"])
        t.outsideDependencies = [
            { name: "lez_core", version: "0.4.0", installed: true, repository: "" },
            { name: "wallet", version: "1.0.0", installed: false, repository: "Logos official" }]
        dlg.trust = t
        tryVerify(function() { return shown(dlg.contentItem, "outsideDependencies") })
        var notice = findChild(dlg.contentItem, "outsideDependencies")
        compare(notice.message.split("\n"), [
            "This needs “lez_core”, which its repository doesn't provide. It will use the copy already installed (0.4.0).",
            "This needs “wallet”, which its repository doesn't provide. It will be installed from Logos official."])
        compare(notice.severity, LogosNotice.Warning)
        // It informs; it doesn't hold the install back.
        var confirm = findChild(dlg.footer, "confirmationDialog.installGate.confirm")
        compare(confirm.enabled, true)
        compare(confirm.text, "Install")
    }

    // Every package gets its badge on its own row; the summary under them says
    // what is wrong once.
    function test_each_row_says_who_signed_it() {
        var dlg = openGate("amm_ui", [change("amm_module"), change("lez_core"), change("mystery")])
        dlg.trust = { needsPrompt: true, blocked: false,
            packages: [{ name: "lez_core", version: "0.4.3", status: "unsigned" },
                       { name: "mystery", version: "1.0.0", status: "unknown" }], publishers: [],
            byName: { amm_ui: { status: "trusted", publisher: "Logos" },
                      amm_module: { status: "trusted", publisher: "Logos" },
                      lez_core: { status: "unsigned" },
                      mystery: { status: "unknown" } } }
        tryVerify(function() { return shown(dlg.contentItem, "confirmationDialog.trust.mystery") })
        compare(bodyOf(dlg).indexOf("Install 'amm_ui' v1.0.0, signed by Logos."), 0, bodyOf(dlg))
        compare(mark(dlg.contentItem, "confirmationDialog.trust.amm_module"), "check | Signed by Logos")
        compare(mark(dlg.contentItem, "confirmationDialog.trust.lez_core"),
                "warning | Not signed: nobody vouches for it")
        compare(mark(dlg.contentItem, "confirmationDialog.trust.mystery"), "warning | Couldn't check who signed it")
        compare(noticeTitle(dlg.contentItem), "Not everything here is from a publisher you trust.")
        compare(noticeLines(dlg.contentItem), ["Install only if you trust its source."])
        compare(findChild(dlg.footer, "confirmationDialog.installGate.confirm").text, "Install anyway")
    }

    // An optional package's row says it too, so unchecking an unsigned one
    // is an informed choice.
    function test_an_optional_row_says_who_signed_it() {
        var dlg = createTemporaryObject(dialogComp, testCase)
        dlg.openWithInstallGate("eth_rpc_ui", "0.1.1",
            [{ name: "verified_proxy_module", optional: true, version: "0.1.4", action: "install" }],
            "package_manager_ui", false)
        dlg.trust = { needsPrompt: true, blocked: false, packages: [], publishers: [],
            byName: { eth_rpc_ui: { status: "trusted", publisher: "Logos" },
                      verified_proxy_module: { status: "unsigned" } } }
        tryVerify(function() { return shown(dlg.contentItem, "packageRow.trust.verified_proxy_module") })
        compare(mark(dlg.contentItem, "packageRow.trust.verified_proxy_module"),
                "warning | Not signed: nobody vouches for it")
        compare(bodyOf(dlg).indexOf("Install 'eth_rpc_ui' v0.1.1, signed by Logos."), 0, bodyOf(dlg))
    }

    // One prompt for the whole install, and the publisher it could trust.
    function test_gate_asks_once_for_everything_not_trusted() {
        var dlg = openGate("chat", [change("chat_module"), change("rln")])
        continueSpy.target = dlg
        continueSpy.clear()
        trustContinueSpy.target = dlg
        trustContinueSpy.clear()
        dlg.trust = untrusted()

        var confirm = findChild(dlg.footer, "confirmationDialog.installGate.confirm")
        compare(confirm.text, "Install anyway")
        compare(confirm.enabled, true)
        compare(confirm.variant, LogosButton.Variant.Secondary)
        tryVerify(function() { return shown(dlg.contentItem, "confirmationDialog.trust.rln") })
        compare(bodyOf(dlg).indexOf("Install 'chat' v1.0.0, signed by a publisher you don't trust yet."), 0,
                bodyOf(dlg))
        compare(mark(dlg.contentItem, "confirmationDialog.trust.chat_module"),
                "warning | Signed by a publisher you don't trust yet")
        compare(mark(dlg.contentItem, "confirmationDialog.trust.rln"), "warning | Not signed: nobody vouches for it")
        compare(noticeLines(dlg.contentItem), ["Install only if you trust its source.",
                                               "Trusting “Other” lets what it signs install without asking from now on.",
                                               "“Other” · key AE21 6C2E F524 7A37 · chat, chat_module"])
        compare(findChild(dlg.contentItem, "installTrust.warning").severity, LogosNotice.Warning)

        var trustButton = findChild(dlg.footer, "confirmationDialog.installGate.trustAndConfirm")
        verify(trustButton.visible)
        compare(trustButton.text, "Trust & install anyway", "rln stays unsigned")
        mouseClick(trustButton)
        compare(trustContinueSpy.count, 1)
        compare(trustContinueSpy.signalArguments[0][0], "chat")
        compare(continueSpy.count, 0)
    }

    function test_gate_install_anyway_trusts_nobody() {
        var dlg = openGate()
        continueSpy.target = dlg
        continueSpy.clear()
        trustContinueSpy.target = dlg
        trustContinueSpy.clear()
        dlg.trust = untrusted()
        mouseClick(findChild(dlg.footer, "confirmationDialog.installGate.confirm"))
        compare(continueSpy.count, 1)
        compare(trustContinueSpy.count, 0)
    }

    // Where trusting would settle everything (the summary's trustCoversAll,
    // install_trust_test's whatTrustingWouldSettle), the other answer is a
    // plain "Install", as "Allow" sits beside "Always allow"; elsewhere it is
    // "Install anyway".
    // Trusting does not vouch for what nobody signed, so then "Trust &
    // install" says "anyway" too.
    function test_anyway_only_where_trust_would_not_cover_it() {
        var dlg = openGate("chat", [change("chat_module")])
        dlg.trust = trustable()
        var confirm = findChild(dlg.footer, "confirmationDialog.installGate.confirm")
        var trustButton = findChild(dlg.footer, "confirmationDialog.installGate.trustAndConfirm")
        compare(confirm.text, "Install")
        compare(confirm.variant, LogosButton.Variant.Secondary)
        verify(trustButton.visible)
        compare(trustButton.text, "Trust & install")
        dlg.trust = untrusted()
        compare(confirm.text, "Install anyway")
        compare(trustButton.text, "Trust & install anyway")

        var upgrade = createTemporaryObject(dialogComp, testCase)
        upgrade.openWithUpgrade("chat", "2.0.0", 0, [], [], [], "package_manager_ui", false)
        upgrade.trust = trustable()
        compare(findChild(upgrade.footer, "confirmationDialog.upgradeCascade.confirm").text, "Upgrade")
        var upgradeTrust = findChild(upgrade.footer, "confirmationDialog.upgradeCascade.trustAndConfirm")
        compare(upgradeTrust.text, "Trust & upgrade")
        upgrade.trust = untrusted()
        compare(upgradeTrust.text, "Trust & upgrade anyway")
    }

    // Nobody signed it, so there is nobody to trust: "anyway" is the only way
    // on, and the box says what it means.
    function test_unsigned_offers_no_publisher_to_trust() {
        var dlg = openGate()
        dlg.trust = unsignedOnly()
        var confirm = findChild(dlg.footer, "confirmationDialog.installGate.confirm")
        compare(confirm.text, "Install anyway")
        compare(confirm.variant, LogosButton.Variant.Primary)
        verify(!shown(dlg.footer, "confirmationDialog.installGate.trustAndConfirm"))
        compare(noticeTitle(dlg.contentItem), "Not everything here is from a publisher you trust.")
        compare(noticeLines(dlg.contentItem), ["Install only if you trust its source."])
    }

    // Where "Trust & install" is offered, the box says what trusting does:
    // it used to be said only in Settings.
    function test_trusting_says_what_it_means() {
        var dlg = openGate()
        dlg.trust = untrusted()
        compare(noticeLines(dlg.contentItem)[1],
                "Trusting “Other” lets what it signs install without asking from now on.")
        var t = untrusted()
        t.trust = publisher("", ["chat"])
        dlg.trust = t
        compare(noticeLines(dlg.contentItem)[1],
                "Trusting key AE21 6C2E F524 7A37 lets what it signs install without asking from now on.")
        t = untrusted()
        t.canTrust = false
        dlg.trust = t
        verify(noticeLines(dlg.contentItem).every(function(line) { return line.indexOf("Trusting") !== 0 }))
    }

    // An update signed by someone else than the version installed says so,
    // in red, even when its new signer is trusted: it used to install
    // without a word.
    function test_a_change_of_signer_is_shown() {
        var dlg = openGate("chat", [change("chat_module")])
        dlg.trust = { needsPrompt: true, blocked: false, canTrust: false, trustCoversAll: false,
            packages: [{ name: "chat_module", version: "2.0.0", status: "trusted", replaces: "Third" }],
            publishers: [],
            byName: { chat_module: { status: "trusted", publisher: "Acme", replaces: "Third" } } }
        compare(noticeTitle(dlg.contentItem), "Something here changes who signed it.")
        compare(noticeLines(dlg.contentItem), ["Install only if you trust its source.",
                                               "chat_module is not signed by Third, who signed the version you have."])
        compare(findChild(dlg.contentItem, "installTrust.warning").severity, LogosNotice.Error)
        tryVerify(function() { return shown(dlg.contentItem, "confirmationDialog.trust.chat_module") })
        compare(mark(dlg.contentItem, "confirmationDialog.trust.chat_module"),
                "warning | Not signed by Third, who signed the version you have")
        verify(!shown(dlg.footer, "confirmationDialog.installGate.trustAndConfirm"))

        // Replacing a copy nobody signed says that instead.
        dlg.trust = { needsPrompt: true, blocked: false, canTrust: false, trustCoversAll: false,
            packages: [{ name: "chat_module", version: "2.0.0", status: "trusted", replacesUnverified: true }],
            publishers: [],
            byName: { chat_module: { status: "trusted", publisher: "Acme", replacesUnverified: true } } }
        compare(noticeLines(dlg.contentItem)[1],
                "chat_module: the version you have isn't signed, so this may not be from the same publisher.")
        compare(mark(dlg.contentItem, "confirmationDialog.trust.chat_module"),
                "warning | The version you have isn't signed, so this may not be from the same publisher")

        // The sentence about the package itself says so too.
        dlg.trust = { needsPrompt: true, blocked: false, canTrust: false, trustCoversAll: false,
            packages: [{ name: "chat", version: "1.0.0", status: "trusted", replaces: "Third" }],
            publishers: [],
            byName: { chat: { status: "trusted", publisher: "Acme", replaces: "Third" } } }
        compare(bodyOf(dlg).indexOf("Install 'chat' v1.0.0, which isn't signed by Third, who signed the version you have."),
                0, bodyOf(dlg))
        dlg.trust = { needsPrompt: true, blocked: false, canTrust: false, trustCoversAll: false,
            packages: [{ name: "chat", version: "1.0.0", status: "trusted", replacesUnverified: true }],
            publishers: [],
            byName: { chat: { status: "trusted", publisher: "Acme", replacesUnverified: true } } }
        compare(bodyOf(dlg).indexOf("Install 'chat' v1.0.0, which may not be from the same publisher as the version you have."),
                0, bodyOf(dlg))
    }

    // What ships with Basecamp only Logos may replace: the box says that, not
    // that the package is damaged.
    function test_what_ships_with_basecamp_is_logoss() {
        var dlg = openGate("chat", [change("capability_module")])
        dlg.trust = { needsPrompt: true, blocked: true, canTrust: false, trustCoversAll: false,
            packages: [{ name: "capability_module", version: "9.0.0", status: "invalid", reserved: true }],
            publishers: [],
            byName: { capability_module: { status: "invalid", reserved: true } } }
        compare(noticeTitle(dlg.contentItem), "This can't be installed.")
        compare(noticeLines(dlg.contentItem),
                ["Comes with Basecamp, so only Logos can update it: capability_module"])
        compare(findChild(dlg.footer, "confirmationDialog.installGate.confirm").enabled, false)
        tryVerify(function() { return shown(dlg.contentItem, "confirmationDialog.trust.capability_module") })
        compare(mark(dlg.contentItem, "confirmationDialog.trust.capability_module"),
                "warning | Comes with Basecamp, so only Logos can update it")

        // About the package itself, not "damaged".
        var gate = openGate("capability_module")
        gate.trust = { needsPrompt: true, blocked: true, canTrust: false, trustCoversAll: false,
            packages: [{ name: "capability_module", version: "1.0.0", status: "invalid", reserved: true }],
            publishers: [],
            byName: { capability_module: { status: "invalid", reserved: true } } }
        compare(bodyOf(gate).indexOf("Install 'capability_module' v1.0.0, which comes with Basecamp, so only Logos can update it."),
                0, bodyOf(gate))
    }

    // One of Basecamp's own apps never installs, Logos-signed or not: the box
    // says it updates only with Basecamp, not that it is damaged.
    function test_basecamps_own_apps_never_install() {
        var dlg = openGate("chat", [change("package_manager_ui")])
        dlg.trust = { needsPrompt: true, blocked: true, canTrust: false, trustCoversAll: false,
            packages: [{ name: "package_manager_ui", version: "9.0.0", status: "invalid", builtIn: true }],
            publishers: [],
            byName: { package_manager_ui: { status: "invalid", builtIn: true } } }
        compare(noticeTitle(dlg.contentItem), "This can't be installed.")
        compare(noticeLines(dlg.contentItem),
                ["Comes with Basecamp and updates only with it: package_manager_ui"])
        compare(findChild(dlg.footer, "confirmationDialog.installGate.confirm").enabled, false)
        tryVerify(function() { return shown(dlg.contentItem, "confirmationDialog.trust.package_manager_ui") })
        compare(mark(dlg.contentItem, "confirmationDialog.trust.package_manager_ui"),
                "warning | Comes with Basecamp and updates only with it")

        var gate = openGate("package_manager_ui")
        gate.trust = { needsPrompt: true, blocked: true, canTrust: false, trustCoversAll: false,
            packages: [{ name: "package_manager_ui", version: "1.0.0", status: "invalid", builtIn: true }],
            publishers: [],
            byName: { package_manager_ui: { status: "invalid", builtIn: true } } }
        compare(bodyOf(gate).indexOf("Install 'package_manager_ui' v1.0.0, which comes with Basecamp and updates only with it."),
                0, bodyOf(gate))
    }

    function test_gate_lets_nothing_damaged_through() {
        var dlg = openGate("chat", [change("storage_module")])
        dlg.trust = blocked()
        compare(findChild(dlg.footer, "confirmationDialog.installGate.confirm").enabled, false)
        verify(!shown(dlg.footer, "confirmationDialog.installGate.trustAndConfirm"))
        // Nothing to install "anyway": no word about choosing to.
        compare(noticeTitle(dlg.contentItem), "This can't be installed.")
        compare(noticeLines(dlg.contentItem), ["“Other” · key AE21 6C2E F524 7A37 · chat, chat_module",
                                               "Damaged, or the signature doesn't hold: storage_module"])
        compare(findChild(dlg.contentItem, "installTrust.warning").severity, LogosNotice.Error)
        tryVerify(function() { return shown(dlg.contentItem, "confirmationDialog.trust.storage_module") })
        compare(mark(dlg.contentItem, "confirmationDialog.trust.storage_module"),
                "warning | Damaged, or its signature doesn't hold")
        var damaged = findChild(findChild(dlg.contentItem, "confirmationDialog.trust.storage_module"),
                                "installTrust.markIcon")
        verify(Qt.colorEqual(damaged.color, Theme.palette.error), "damaged reads as an error: " + damaged.color)
    }

    // The dialog is reused: what was said about the last install must not
    // hold up, or wave through, the next one.
    function test_reopening_forgets_the_last_assessment() {
        var dlg = openGate()
        dlg.trust = blocked()
        dlg.close()
        dlg.openWithInstallGate("wallet", "1.0.0", [], "package_manager_ui", false)
        compare(findChild(dlg.footer, "confirmationDialog.installGate.confirm").enabled, true)
        verify(!shown(dlg.contentItem, "installTrust.warning"))
        dlg.close()
        dlg.openWithUpgrade("wallet", "2.0.0", 0, [], [], [], "package_manager_ui", false)
        compare(findChild(dlg.footer, "confirmationDialog.upgradeCascade.confirm").enabled, true)
    }

    function test_upgrade_asks_in_its_own_words() {
        var dlg = createTemporaryObject(dialogComp, testCase)
        dlg.openWithUpgrade("chat", "2.0.0", 0, [], [], [], "package_manager_ui", false)
        dlg.trust = untrusted()
        var confirm = findChild(dlg.footer, "confirmationDialog.upgradeCascade.confirm")
        var trustButton = findChild(dlg.footer, "confirmationDialog.upgradeCascade.trustAndConfirm")
        compare(confirm.text, "Upgrade anyway")
        compare(trustButton.text, "Trust & upgrade anyway")
        compare(bodyOf(dlg).indexOf("Upgrade 'chat' to v2.0.0, signed by a publisher you don't trust yet."), 0,
                bodyOf(dlg))

        dlg.close()
        dlg.openWithUpgrade("chat", "0.9.0", 1, [], [], [], "package_manager_ui", false)
        var two = untrusted()
        two.publishers.push({ did: otherDid + "2", name: "Another", packages: ["rln"] })
        dlg.trust = two
        compare(confirm.text, "Downgrade anyway")
        compare(trustButton.text, "Trust & downgrade anyway")
    }

    // A key calling itself Logos is said to be what it is, and only "Install
    // anyway" is offered for it: trusting it is not one click away.
    function test_a_publisher_copying_a_trusted_name_is_not_offered() {
        var dlg = openGate("chat", [change("chat_module")])
        var t = untrusted()
        t.publishers = [publisher("Logos", ["chat", "chat_module"], "Logos")]
        t.canTrust = false
        t.byName.chat = { status: "untrusted", publisher: "Logos", impersonates: "Logos" }
        dlg.trust = t
        var line = noticeLines(dlg.contentItem)[1]
        verify(line.indexOf("Claims to be “Logos”, but its key (AE21 6C2E F524 7A37) is not Logos's") === 0, line)
        compare(findChild(dlg.contentItem, "installTrust.warning").severity, LogosNotice.Error)
        compare(bodyOf(dlg).indexOf("Install 'chat' v1.0.0, which claims to be Logos but isn't signed with its key."),
                0, bodyOf(dlg))
        t.byName.chat_module = { status: "untrusted", publisher: "Logos", impersonates: "Logos" }
        dlg.trust = Object.assign({}, t)
        tryVerify(function() { return shown(dlg.contentItem, "confirmationDialog.trust.chat_module") })
        compare(mark(dlg.contentItem, "confirmationDialog.trust.chat_module"),
                "warning | Claims to be Logos, but isn't signed with its key")
        verify(!shown(dlg.footer, "confirmationDialog.installGate.trustAndConfirm"))
        compare(findChild(dlg.footer, "confirmationDialog.installGate.confirm").text, "Install anyway")

        // Beside one it can trust, that one is offered.
        t.publishers.push({ did: otherDid + "2", fingerprint: "1111 2222 3333 4444", name: "Fine",
                            packages: ["rln"], impersonates: "" })
        t.canTrust = true
        dlg.trust = t
        var trustButton = findChild(dlg.footer, "confirmationDialog.installGate.trustAndConfirm")
        verify(trustButton.visible)
        compare(trustButton.text, "Trust & install anyway", "the lookalike stays untrusted")
    }

    // A touch screen has no hover: a tap on a row's mark shows what it means,
    // and another hides it. Where the words are written out beside the mark,
    // a tap shows nothing more.
    function test_a_tap_shows_what_a_mark_means() {
        var dlg = openGate("chat", [change("chat_module")])
        dlg.trust = untrusted()
        tryVerify(function() { return shown(dlg.contentItem, "confirmationDialog.trust.chat_module") })
        waitForRendering(dlg.contentItem)
        var markItem = findChild(dlg.contentItem, "confirmationDialog.trust.chat_module")
        var tip = findChild(markItem, "installTrust.markTip")
        compare(tip.visible, false)
        var touch = touchEvent(markItem)
        touch.press(0, markItem).commit()
        touch.release(0, markItem).commit()
        tryCompare(tip, "visible", true)
        touch.press(0, markItem).commit()
        touch.release(0, markItem).commit()
        tryCompare(tip, "visible", false)

        var line = createTemporaryObject(lineComp, testCase, { verdict: untrusted().byName.chat })
        waitForRendering(line)
        var lineMark = findChild(line, "installTrust.lineMark")
        var lineTip = findChild(lineMark, "installTrust.markTip")
        touch = touchEvent(lineMark)
        touch.press(0, lineMark).commit()
        touch.release(0, lineMark).commit()
        wait(50)
        compare(lineTip.visible, false)
    }

    // A package already installed and left as it is says what it is, muted:
    // it is not part of what is being decided.
    function test_an_installed_row_says_what_it_is() {
        var dlg = openGate("storage_ui", [
            { name: "storage_module", optional: true, version: "3.0.2", installedVersion: "3.0.2",
              action: "installed" },
            { name: "old_dep", optional: true, version: "1.0.0", installedVersion: "1.0.0",
              action: "installed" }])
        dlg.trust = { needsPrompt: true, blocked: false, packages: [], publishers: [],
                      byName: { storage_ui: { status: "unsigned" },
                                storage_module: { status: "bundled", installed: true },
                                old_dep: { status: "unsigned", installed: true } } }
        tryVerify(function() { return shown(dlg.contentItem, "packageRow.trust.old_dep") })
        compare(mark(dlg.contentItem, "packageRow.trust.storage_module"), "check | Ships with Basecamp")
        compare(mark(dlg.contentItem, "packageRow.trust.old_dep"), "warning | Already installed, not signed")
        var muted = findChild(dlg.contentItem, "packageRow.trust.old_dep")
        var icon = findChild(muted, "installTrust.markIcon")
        verify(Qt.colorEqual(icon.color, Theme.palette.textTertiary), "muted: " + icon.color)
    }

    // A name next to its mark is shown in full when there is room: the rows
    // size it to its own width, which a layout rounds down.
    function test_a_short_name_beside_its_mark_is_shown_in_full() {
        var dlg = openGate("storage_ui", [{ name: "logos_dep", displayName: "Logos", optional: true,
                                            version: "1.0.0", action: "install" }])
        dlg.trust = signedByLogos(["logos_dep"])
        tryVerify(function() { return shown(dlg.contentItem, "packageRow.trust.logos_dep") })
        waitForRendering(dlg.contentItem)
        verify(!findChild(dlg.contentItem, "packageRow.name.logos_dep").truncated)

        var gate = openGate("app", [{ name: "Logos", action: "install", toVersion: "", fromVersion: "" }])
        gate.trust = signedByLogos(["Logos"])
        tryVerify(function() { return shown(gate.contentItem, "confirmationDialog.trust.Logos") })
        waitForRendering(gate.contentItem)
        verify(!findChild(gate.contentItem, "confirmationDialog.change.Logos").truncated)
    }

    // A row's mark sits right after its name, inside the row's name column.
    function test_a_rows_mark_sits_beside_its_name() {
        var dlg = openGate("storage_ui", [{ name: "storage_module", optional: true, version: "0.4.0",
                                            action: "install", versions: [{ manifest: { version: "0.4.0" } }] }])
        dlg.trust = { needsPrompt: true, blocked: false, packages: [], publishers: [],
                      byName: { storage_ui: { status: "unsigned" },
                                storage_module: { status: "unsigned" } } }
        tryVerify(function() { return shown(dlg.contentItem, "packageRow.trust.storage_module") })
        waitForRendering(dlg.contentItem)
        var markItem = findChild(dlg.contentItem, "packageRow.trust.storage_module")
        var version = findChild(dlg.contentItem, "packageRow.version.storage_module")
        var markRight = markItem.mapToItem(null, markItem.width, 0).x
        verify(markRight <= version.mapToItem(null, 0, 0).x,
               "mark ends at " + markRight + ", past the version picker")
    }

    // What a publisher calls itself is shown as it is: markup is not drawn,
    // and a name reading "%2" is not filled in.
    function test_a_publishers_name_is_shown_as_text() {
        var dlg = openGate("chat", [change("chat_module")])
        var t = untrusted()
        t.publishers = [publisher("<b>Logos</b> %2", ["chat"])]
        t.byName.chat = { status: "trusted", publisher: "<b>Acme</b>" }
        dlg.trust = t
        compare(findChild(dlg.contentItem, "installTrust.warning").messageItem.textFormat, TextEdit.PlainText)
        compare(noticeLines(dlg.contentItem)[2], "“<b>Logos</b> %2” · key AE21 6C2E F524 7A37 · chat")
        compare(bodyOf(dlg).indexOf("Install 'chat' v1.0.0, signed by <b>Acme</b>."), 0, bodyOf(dlg))
        compare(findChild(dlg.contentItem, "confirmationDialog.body").textFormat, Text.PlainText)

        t.byName.chat_module = { status: "trusted", publisher: "<i>Acme</i>" }
        dlg.trust = Object.assign({}, t)
        tryVerify(function() { return shown(dlg.contentItem, "confirmationDialog.trust.chat_module") })
        var markItem = findChild(dlg.contentItem, "confirmationDialog.trust.chat_module")
        compare(markItem.description, "Signed by <i>Acme</i>")
        compare(findChild(markItem, "installTrust.markTip").labelItem.textFormat, Text.PlainText)

        // One that calls itself nothing is called that.
        t.publishers = [publisher("", ["chat"])]
        dlg.trust = Object.assign({}, t)
        compare(noticeLines(dlg.contentItem)[2], "Unnamed publisher · key AE21 6C2E F524 7A37 · chat")
    }

    // A catalog's own text sits beside the trust marks: drawn as markup, a name
    // or description could fake one ("✓ Signed by Logos").
    function test_a_catalogs_own_text_is_never_markup() {
        var fake = "Wallet <font color='green'>✓ Signed by Logos</font>"
        var gate = openGate("wallet", [{ name: fake, action: "install", toVersion: "1.0.0", fromVersion: "" },
                                       { name: "dep", displayName: fake, description: fake, optional: true,
                                         version: "1.0.0", action: "install" }])
        gate.trust = untrusted()
        tryVerify(function() { return !!findChild(gate.contentItem, "confirmationDialog.change." + fake) })
        compare(findChild(gate.contentItem, "confirmationDialog.change." + fake).textFormat, Text.PlainText)
        tryVerify(function() { return !!findChild(gate.contentItem, "packageRow.name.dep") })
        compare(findChild(gate.contentItem, "packageRow.name.dep").textFormat, Text.PlainText)
        compare(findChild(gate.contentItem, "packageRowDescription").textFormat, Text.PlainText)
    }

    // Any app may ask for an install, and names itself: drawn as markup, its
    // name could hide that it is not part of Logos; filled twice, a "%2" in
    // it would take the package id's place.
    function test_the_requesters_name_is_shown_as_text() {
        var dlg = createTemporaryObject(dialogComp, testCase)
        dlg.displayNameLookup = function(name) {
            return name === "evil_ui" ? "<font color='white'>Package Manager</font> %2" : name
        }
        dlg.openWithInstallGate("chat", "1.0.0", [], "evil_ui", false)
        var line = findChild(dlg.contentItem, "confirmationDialog.requester")
        compare(line.textFormat, Text.PlainText)
        compare(line.text, "Requested by <font color='white'>Package Manager</font> %2 (evil_ui), "
                           + "which is not part of Logos.")
    }

    // ── App Manager ──

    // ── Settings → Trusted Publishers ──

    function logosRow() {
        return { name: "logos", did: "did:jwk:logos", displayName: "Logos", url: "https://logos.co",
                 fingerprint: "4BBC C11D CB35 F9FD", builtIn: true, addedAt: "2026-10-01T10:00:00Z" }
    }

    function otherRow() {
        return { name: "publisher-0123456789abcdef", did: otherDid, displayName: "Other",
                 fingerprint: "AE21 6C2E F524 7A37", url: "https://other.example", builtIn: false,
                 addedAt: "2026-11-02T10:00:00Z" }
    }

    function openPublishers(rows) {
        var view = createTemporaryObject(publishersComp, testCase, { publishers: rows })
        removeSpy.target = view
        removeSpy.clear()
        tryVerify(function() { return !!findChild(view, "trustedPublishers.remove." + rows[rows.length - 1].name) })
        waitForRendering(view)
        return view
    }

    function confirmDelete(view, keyName) {
        mouseClick(findChild(view, "trustedPublishers.remove." + keyName))
        var dialog = findChild(view, "trustedPublishers.removeConfirm")
        tryCompare(dialog, "opened", true)
        // Until laid out, Cancel and Delete sit on top of each other.
        waitForRendering(testCase)
        mouseClick(findChild(dialog, "trustedPublishers.removeConfirm.confirm"))
    }

    // Every publisher can be deleted, Logos too; none is switched on or off.
    function test_every_publisher_can_be_deleted() {
        var view = openPublishers([logosRow(), otherRow()])
        verify(shown(view, "trustedPublishers.remove.logos"))
        verify(shown(view, "trustedPublishers.remove.publisher-0123456789abcdef"))
        verify(!findChild(view, "trustedPublishers.switch.logos"))

        confirmDelete(view, "logos")
        compare(removeSpy.count, 1)
        compare(removeSpy.signalArguments[0][0], "logos")

        // A layout hands out whole pixels: "Logos" once lost its last letter
        // to a fraction of one with the row half empty.
        verify(!findChild(view, "trustedPublishers.name.logos").truncated, "Logos is shown in full")
    }

    // A row's controls keep to their own columns: a Remove button once spilled
    // out of a cell too narrow for it, over the key.
    function test_row_controls_do_not_overlap() {
        var view = openPublishers([logosRow(), otherRow()])
        var right = 0
        var names = ["fingerprint", "copy", "remove"]
        for (var i = 0; i < names.length; ++i) {
            var item = findChild(view, "trustedPublishers." + names[i] + ".publisher-0123456789abcdef")
            var at = item.mapToItem(view, 0, 0)
            verify(at.x >= right, names[i] + " starts after what is left of it")
            right = at.x + item.width
        }
        verify(right <= view.width, "the delete button is inside the view")
    }

    // At a phone's width the key goes under the publisher's name; there its
    // copy button, touch-sized, is centred on the key, not hanging below it.
    function test_a_key_under_the_name_lines_up_with_its_copy_button() {
        Theme.viewWidth = 300
        var view = createTemporaryObject(publishersComp, testCase, { publishers: [logosRow(), otherRow()], width: 300 })
        tryVerify(function() { return !!findChild(view, "trustedPublishers.copy.publisher-0123456789abcdef") })
        waitForRendering(view)
        var name = findChild(view, "trustedPublishers.name.publisher-0123456789abcdef")
        var key = findChild(view, "trustedPublishers.fingerprint.publisher-0123456789abcdef")
        var copy = findChild(view, "trustedPublishers.copy.publisher-0123456789abcdef")
        Theme.viewWidth = 0
        verify(key.mapToItem(view, 0, 0).y >= name.mapToItem(view, 0, name.height).y, "under the name")
        fuzzyCompare(key.mapToItem(view, 0, key.height / 2).y, copy.mapToItem(view, 0, copy.height / 2).y, 1)
    }

    // Under the name, a key's copy button is at the right, its edge on the
    // delete button's above it; it had stopped short, beside the key.
    function test_a_key_under_the_name_keeps_its_copy_button_at_the_right() {
        Theme.viewWidth = 300
        try {
            var view = createTemporaryObject(publishersComp, testCase, { publishers: [logosRow(), otherRow()], width: 300 })
            tryVerify(function() { return !!findChild(view, "trustedPublishers.copy.publisher-0123456789abcdef") })
            waitForRendering(view)
            var name = findChild(view, "trustedPublishers.name.publisher-0123456789abcdef")
            var copy = findChild(view, "trustedPublishers.copy.publisher-0123456789abcdef")
            var remove = findChild(view, "trustedPublishers.remove.publisher-0123456789abcdef")
            verify(copy.mapToItem(view, 0, 0).y >= name.mapToItem(view, 0, name.height).y, "the key is under the name")
            fuzzyCompare(copy.mapToItem(view, copy.width, 0).x, remove.mapToItem(view, remove.width, 0).x, 1)
        } finally {
            Theme.viewWidth = 0
        }
    }

    // A row says since when its publisher is trusted, so an entry nobody
    // remembers adding stands out. Logos is built in.
    function test_a_publisher_says_since_when_it_is_trusted() {
        var view = openPublishers([logosRow(), otherRow()])
        compare(findChild(view, "trustedPublishers.detail.logos").text, "Built in")
        compare(findChild(view, "trustedPublishers.detail.publisher-0123456789abcdef").text,
                "https://other.example · trusted since 2 Nov 2026")
    }

    // The key is what is trusted: its fingerprint on the row, the whole key a copy away.
    function test_a_publisher_is_shown_by_its_key() {
        var view = openPublishers([logosRow(), otherRow()])
        compare(findChild(view, "trustedPublishers.fingerprint.publisher-0123456789abcdef").text,
                "AE21 6C2E F524 7A37")
        compare(findChild(view, "trustedPublishers.copy.publisher-0123456789abcdef").value, otherDid)
        compare(findChild(view, "trustedPublishers.name.publisher-0123456789abcdef").textFormat,
                Text.PlainText)
    }

    // Deleting a publisher asks first, and says what keeps working and how to
    // trust it again.
    function test_deleting_a_publisher_asks_first() {
        var view = openPublishers([logosRow(), otherRow()])
        view.installedFrom = function(did) { return did === otherDid ? ["chat", "chat_module"] : [] }
        var remove = findChild(view, "trustedPublishers.remove.publisher-0123456789abcdef")
        mouseClick(remove)
        var dialog = findChild(view, "trustedPublishers.removeConfirm")
        tryCompare(dialog, "opened", true)
        waitForRendering(testCase)
        var message = findChild(dialog, "trustedPublishers.removeConfirm.message")
        compare(message.textFormat, Text.PlainText)
        compare(message.text, "Future installs from “Other” will ask first. "
                              + "Apps you installed from it keep working: chat, chat_module. "
                              + "To trust it again, choose “Trust & install” when one of its packages asks.")
        // Its title is rich text, so it names nobody.
        compare(dialog.title, "Delete this publisher?")
        compare(removeSpy.count, 0)

        mouseClick(findChild(dialog, "trustedPublishers.removeConfirm.cancel"))
        tryCompare(dialog, "visible", false)
        compare(removeSpy.count, 0)

        confirmDelete(view, "publisher-0123456789abcdef")
        compare(removeSpy.count, 1)
        compare(removeSpy.signalArguments[0][0], "publisher-0123456789abcdef")
    }

    // While a publisher is being deleted, its delete button waits.
    function test_a_publisher_being_deleted_takes_no_other_change() {
        var busy = otherRow()
        busy.busy = true
        var view = openPublishers([logosRow(), busy])
        var remove = findChild(view, "trustedPublishers.remove.publisher-0123456789abcdef")
        compare(remove.enabled, false)
        mouseClick(remove)
        compare(findChild(view, "trustedPublishers.removeConfirm").opened, false)
        compare(findChild(view, "trustedPublishers.remove.logos").enabled, true)
    }

    // A delete that failed says so; the next one clears it.
    function test_a_failed_change_is_reported() {
        var view = openPublishers([logosRow(), otherRow()])
        var notice = findChild(view, "trustedPublishers.failure")
        compare(notice.visible, false)
        view.reportFailure("Couldn't delete “<b>Other</b>”: disk full")
        compare(notice.visible, true)
        compare(notice.severity, LogosNotice.Error)
        compare(notice.message, "Couldn't delete “<b>Other</b>”: disk full")
        compare(notice.messageItem.textFormat, TextEdit.PlainText)

        confirmDelete(view, "publisher-0123456789abcdef")
        compare(removeSpy.count, 1)
        compare(notice.visible, false)
    }
}
