import QtQuick
import QtTest

import Logos.Theme

// Directory imports: see tst_IntentInstallDialog.qml.
import "../../src/Basecamp/Settings"
import "../../src/Basecamp/Shell"
import Basecamp.AppManager
import Basecamp.Backend 1.0

// Regression guard for logos-basecamp#546: parts of the shell had hardcoded
// colours, so a custom theme left them dark, and light themes put white text
// on a light background. fixtures/themes/test.sample is light: background
// #FAFAFA, text #171717. The C++ chrome is workspace_area_test's.
TestCase {
    id: testCase
    name: "ThemedViews"
    when: windowShown
    width: 900
    height: 900

    Component {
        id: interfaceComp
        PluginInterfaceView { width: testCase.width; height: testCase.height }
    }

    Component {
        id: dialogComp
        ConfirmationDialog {}
    }

    function init() { themeManager.select("dark") }
    function cleanupTestCase() { themeManager.select("dark") }

    function sameColor(a, b) { return Qt.colorEqual(a, b) }

    // ── The QML views: every visible text is drawn in a palette colour ──────

    function paletteColors() {
        const p = Theme.palette
        return Object.keys(p).map(function(k) { return p[k] })
    }

    function visibleTexts(item, out) {
        if (!item || !item.visible) return out
        if (typeof item.text === "string" && item.font !== undefined
                && item.color !== undefined && item.text.length > 0)
            out.push(item)
        for (let i = 0; i < item.children.length; ++i)
            visibleTexts(item.children[i], out)
        return out
    }

    function verifyThemedTexts(root, what) {
        const colors = paletteColors()
        const texts = visibleTexts(root, [])
        verify(texts.length > 0, what + ": no text found")
        for (let i = 0; i < texts.length; ++i) {
            const t = texts[i]
            verify(colors.some(function(c) { return sameColor(c, t.color) }),
                   what + ": \"" + t.text + "\" is " + t.color + ", not a palette colour")
        }
    }

    function test_inspector_detail_is_themed() {
        verify(themeManager.select("test.sample"))
        // In the window, not the (invisible) TestCase, so `visible` means something.
        const view = createTemporaryObject(interfaceComp, testCase.Window.window.contentItem)
        view.methods = [{ name: "ping", signature: "ping()", description: "Answers pong" }]
        view.events = [{ name: "ponged", signature: "ponged()", description: "After a ping" }]
        view.resultText = "pong"
        waitForRendering(view)
        verifyThemedTexts(view, "inspector")

        view.methods = []
        view.events = []
        waitForRendering(view)
        verifyThemedTexts(view, "inspector, no methods")
    }

    function luminance(c) {
        function lin(v) { return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4) }
        return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b)
    }
    function contrast(a, b) {
        const la = luminance(a), lb = luminance(b)
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
    }

    // Signatures sit on the surface card; textTertiary was 4.2:1 there under
    // Dark, below AA's 4.5.
    function test_inspector_signatures_meet_aa_on_their_card() {
        const view = createTemporaryObject(interfaceComp, testCase.Window.window.contentItem)
        view.methods = [{ name: "ping", signature: "ping()", description: "Answers pong" }]
        view.events = [{ name: "ponged", signature: "ponged()", description: "After a ping" }]
        waitForRendering(view)
        const texts = visibleTexts(view, [])
        for (const sig of ["ping()", "ponged()"]) {
            const t = texts.find(x => x.text === sig)
            verify(t, sig + " not shown")
            const ratio = contrast(t.color, Theme.palette.surface)
            verify(ratio >= 4.5, sig + " is " + ratio.toFixed(2) + ":1 on surface")
        }
    }

    function test_confirmation_dialogs_are_themed() {
        verify(themeManager.select("test.sample"))
        const dlg = createTemporaryObject(dialogComp, testCase)

        dlg.openWithInstallError("chat", "Download failed")
        tryVerify(function() { return dlg.opened })
        verifyThemedTexts(dlg.contentItem, "installError")
        dlg.close()
        tryVerify(function() { return !dlg.visible })

        dlg.openWith("missingDeps", "chat", ["delivery_module"], "")
        tryVerify(function() { return dlg.opened })
        verifyThemedTexts(dlg.contentItem, "missingDeps")
        dlg.close()
        tryVerify(function() { return !dlg.visible })

        dlg.openWithUpgrade("chat", "2.0.0", 0, [], ["chat_ui"],
            [{ name: "delivery_module", action: "upgrade", fromVersion: "1.0.0", toVersion: "1.1.0" }],
            "package_manager_ui", false)
        tryVerify(function() { return dlg.opened })
        verifyThemedTexts(dlg.contentItem, "upgradeCascade")
    }
}
