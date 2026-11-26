import QtQuick
import QtQuick.Controls
import QtTest

import Logos.Theme
import Basecamp.Backend 1.0

// Directory import, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"
import "FitChecks.js" as FitChecks

// The confirmations Basecamp shows before a change, and its notices, in a
// phone-wide window: a sheet across the bottom, as the uninstall confirmation
// is, not a 560 px dialog centred past the window's sides. Their buttons stack
// full width with the main one last, and they must be answered: neither a drag
// nor a tap outside closes them; Esc still cancels.
TestCase {
    id: testCase
    name: "ConfirmationDialogSheet"
    width: 390
    height: 800
    // Input reaches nothing in an invisible TestCase, the default.
    visible: true
    when: windowShown

    Component { id: dialogComp; ConfirmationDialog {} }
    Component { id: spyComp; SignalSpy {} }

    readonly property string otherDid: "did:key:z6MkotherOTHERother"

    function changes() {
        return [
            { name: "wallet_module", action: "install", toVersion: "1.2.0", repository: "Logos Apps" },
            { name: "storage_module", action: "upgrade", fromVersion: "0.2.0", toVersion: "0.3.0",
              repository: "Logos Apps" },
            { name: "rln", displayName: "RLN", optional: true, version: "0.1.0", action: "install",
              description: "Rate limiting nullifiers for every message you send",
              versions: [{ manifest: { version: "0.1.0" }, rootHash: "aa", action: "install" }] }
        ]
    }

    function untrusted() {
        return { needsPrompt: true, blocked: false, canTrust: true, trustCoversAll: false,
                 packages: [{ name: "wallet_ui", version: "1.2.0", status: "untrusted", did: otherDid,
                              signerName: "Other" }],
                 publishers: [{ did: otherDid, fingerprint: "AE21 6C2E F524 7A37", name: "Other",
                                url: "https://other.example", packages: ["wallet_ui"], impersonates: "" }],
                 trust: { did: otherDid, fingerprint: "AE21 6C2E F524 7A37", name: "Other",
                          url: "https://other.example", packages: ["wallet_ui"], impersonates: "" },
                 byName: { wallet_ui: { status: "untrusted", publisher: "Other", impersonates: "" } } }
    }

    function open(mode) {
        const dlg = createTemporaryObject(dialogComp, testCase)
        if (mode === "installGate") {
            dlg.openWithInstallGate("wallet_ui", "1.2.0", changes(), "package_manager_ui", false)
            dlg.trust = untrusted()
        } else if (mode === "upgradeCascade") {
            dlg.openWithUpgrade("wallet_ui", "1.2.0", 0, ["wallet_module"], ["chat_ui", "storage_ui"],
                                changes(), "package_manager_ui", false)
        } else if (mode === "missingDeps") {
            dlg.openWith("missingDeps", "wallet_ui",
                         [{ name: "wallet_module", kind: "absent", detail: "not installed" }], "absent")
        } else if (mode === "unloadCascade") {
            dlg.openWith("unloadCascade", "wallet_module", ["wallet_ui", "chat_ui"], "")
        } else if (mode === "installError") {
            dlg.openWithInstallError("wallet_ui", "Download failed: the connection was reset by the peer")
        } else {
            dlg.openWithLoadError("wallet_ui", "qrc:/Main.qml:12:5: Type WalletView unavailable\n"
                                  + "qrc:/WalletView.qml:3:1: module \"Logos.Wallet\" is not installed")
        }
        tryCompare(dlg, "opened", true)
        const overlay = dlg.Overlay.overlay
        tryVerify(() => Math.abs(dlg.y + dlg.height - overlay.height) < 0.5, 2000, "the sheet's bottom is the window's")
        waitForRendering(testCase)
        return dlg
    }

    function overflowOf(item, root) {
        return FitChecks.overflow(item).map(child => FitChecks.describe(child, root)).join("; ")
    }

    function test_a_sheet_across_the_bottom_data() {
        return ["installGate", "upgradeCascade", "missingDeps", "unloadCascade", "installError", "loadFailed"]
               .map(mode => ({ tag: mode, mode: mode }))
    }

    function test_a_sheet_across_the_bottom(data) {
        const dlg = open(data.mode)
        verify(dlg.sheet, "a sheet")
        compare(dlg.x, 0)
        compare(dlg.width, testCase.width)
        compare(overflowOf(dlg.contentItem, dlg.contentItem), "", "the body fits")
        compare(overflowOf(dlg.footer, dlg.footer), "", "the buttons fit")
        verify(!dlg.dragToClose, "it must be answered")
    }

    // Drawn, not only placed: some pixel across its middle is not the sheet's.
    function drawn(item, image, sheet) {
        const left = item.mapToItem(testCase, 0, item.height / 2)
        for (let x = Math.round(left.x); x < Math.round(left.x + item.width); ++x)
            if (String(image.pixel(x, Math.round(left.y))) !== sheet)
                return true
        return false
    }

    // The buttons stack full width, the main one last, where the thumb is,
    // each drawn: hidden until the mode was set, two of them weren't.
    function test_its_buttons_stack_main_last() {
        const dlg = open("installGate")
        const cancel = findChild(dlg.footer, "confirmationDialog.installGate.cancel")
        const anyway = findChild(dlg.footer, "confirmationDialog.installGate.confirm")
        const trust = findChild(dlg.footer, "confirmationDialog.installGate.trustAndConfirm")
        verify(trust.visible)
        compare(anyway.text, "Install anyway")
        compare(cancel.width, anyway.width)
        compare(trust.width, anyway.width)
        verify(cancel.y < anyway.y && anyway.y < trust.y, "Cancel, Install anyway, then Trust & install")
        const image = grabImage(testCase)
        for (const button of [cancel, anyway, trust])
            verify(drawn(button, image, String(dlg.backgroundColor)), button.text + " is drawn")
    }

    // Each list shows all its rows, the last one whole.
    function test_its_lists_show_every_row_data() {
        return [
            { tag: "unloading", mode: "unloadCascade", list: "confirmationDialog.items" },
            { tag: "missing", mode: "missingDeps", list: "confirmationDialog.items" },
            { tag: "running", mode: "upgradeCascade", list: "confirmationDialog.running" },
        ]
    }

    function test_its_lists_show_every_row(data) {
        const dlg = open(data.mode)
        const list = findChild(dlg.body, data.list)
        verify(list && list.count > 0, "the list is there")
        verify(list.contentHeight <= list.height, "rows " + list.contentHeight + " high in " + list.height)
    }

    // A reason too long for a line, as a signer's is, wraps whole.
    function test_a_long_reason_wraps() {
        const did = "did:key:z6MkhaXgBZDvotDkL5257faiztiGiC2QtKLGpbnnEGta2doK"
        const dlg = createTemporaryObject(dialogComp, testCase)
        dlg.openWith("missingDeps", "wallet_ui",
                     [{ name: "wallet_module", kind: "signer",
                        detail: "signed by a different key; requires " + did + ", signed by " + otherDid }], "signer")
        tryCompare(dlg, "opened", true)
        const list = findChild(dlg.body, "confirmationDialog.items")
        tryVerify(() => list.itemAtIndex(0) !== null)
        const row = list.itemAtIndex(0)
        verify(row.contentWidth <= list.width, "the row is " + row.contentWidth + " wide in " + list.width)
        verify(list.contentHeight <= list.height, "the list as tall as its lines")
    }

    // A tap outside leaves it open; Esc cancels, as the backend expects.
    function test_a_tap_outside_leaves_it_and_esc_cancels() {
        const dlg = open("installGate")
        const cancelled = createTemporaryObject(spyComp, testCase, { target: dlg, signalName: "cancelClicked" })
        mouseClick(testCase, testCase.width / 2, 20)
        wait(50)
        verify(dlg.opened, "still open")
        keyClick(Qt.Key_Escape)
        tryCompare(dlg, "visible", false)
        compare(cancelled.count, 1)
        compare(cancelled.signalArguments[0][0], "wallet_ui")
    }

    // Install closes it with the backend told once.
    function test_install_answers_once() {
        const dlg = open("installGate")
        const confirmed = createTemporaryObject(spyComp, testCase, { target: dlg, signalName: "continueClicked" })
        const cancelled = createTemporaryObject(spyComp, testCase, { target: dlg, signalName: "cancelClicked" })
        mouseClick(findChild(dlg.footer, "confirmationDialog.installGate.confirm"))
        tryCompare(dlg, "visible", false)
        compare(confirmed.count, 1)
        compare(cancelled.count, 0, "not cancelled on the way out")
    }
}
