import QtQuick
import QtTest

import Logos.Theme
import Basecamp.Backend
import BasecampTests

import "../../src/Basecamp/Shell"
import "FitChecks.js" as FitChecks

// The welcome page from a phone to a desktop: the margins are the page's,
// the two cards sit side by side while both keep their width (LogosCardGrid),
// the greeting steps down with a phone's titles (displayText) and wraps where
// it must, and Recently Closed wraps instead of running off the side. The
// desktop looks as it did.
TestCase {
    id: testCase
    name: "WelcomePage"
    width: 1280
    height: 800
    visible: true
    when: windowShown

    readonly property string repo: "https://example.test/logos-repo.json"

    function app(name, displayName, type) {
        return { name: name, repositoryUrl: repo, displayName: displayName,
                 description: displayName + " for Logos.", category: "Apps",
                 type: type, versions: [{ version: "1.0.0" }] }
    }

    TestAppsModel { id: apps }

    // The host's backend, as the page reads it.
    property var backend: QtObject {
        property var recentlyClosedApps: [
            { name: "wallet_ui", displayName: "Wallet" }, { name: "chat_ui", displayName: "Chat" },
            { name: "storage_ui", displayName: "Storage" }, { name: "notes_ui", displayName: "Notes" },
            { name: "mail_ui", displayName: "Mail" }]
        property var appsModel: apps
        property bool appsLoading: false
        property bool packageCatalog: true
        function requestCatalog() {}
    }

    Component { id: pageComp; WelcomePage {} }

    function initTestCase() {
        const rows = []
        for (const n of ["Wallet", "Chat", "Storage", "Notes", "Mail", "Swap", "Calendar"])
            rows.push(app(n.toLowerCase() + "_ui", n, "ui_qml"))
        rows.push(app("waku_module", "Waku", "core"))
        apps.setCatalog(rows)
    }

    function init() {
        failOnWarning(/Binding loop/)
    }

    function cleanup() {
        Theme.viewWidth = 0
        Theme.hasKeyboard = true
    }

    function makePage(w) {
        Theme.viewWidth = w
        const page = createTemporaryObject(pageComp, testCase, { width: w, height: testCase.height })
        verify(page)
        waitForRendering(page)
        return page
    }

    function find(page, name) {
        const item = findChild(page, name)
        verify(item, name)
        return item
    }

    function at(item, page) {
        return item.mapToItem(page, 0, 0)
    }

    // ── Nothing past the sides ──

    // A page taller than its view scrolls to its end: on a phone the cards
    // stack and the recents wrap, and below the first screen was unreachable.
    function test_a_phone_page_scrolls_to_its_end() {
        Theme.viewWidth = 390
        const page = createTemporaryObject(pageComp, testCase, { width: 390, height: 600 })
        waitForRendering(page)
        const scroller = find(page, "welcomePage.scroller")
        const flick = scroller.contentItem
        verify(flick.contentHeight > scroller.height, "content " + flick.contentHeight + " in " + scroller.height)
        flick.contentY = flick.contentHeight - flick.height
        waitForRendering(page)
        const recents = find(page, "welcomePage.recentApps")
        const bottom = recents.mapToItem(scroller, 0, recents.height).y
        verify(bottom <= scroller.height + 0.5, "the recents' end in view, at " + bottom)
    }

    function test_fits_data() {
        const out = []
        for (const w of [360, 390, 600, 700, 900, 1280])
            for (const searching of [false, true])
                out.push({ tag: w + (searching ? " searching" : ""), width: w, searching: searching })
        return out
    }

    function test_fits(data) {
        const page = makePage(data.width)
        if (data.searching) {
            find(page, "welcomePage.search").text = "a"
            waitForRendering(page)
        }
        const over = FitChecks.overflow(page)
        compare(over.map(item => FitChecks.describe(item, page)).join("; "), "", "past the sides")
    }

    // ── Desktop: as it was ──

    function test_desktop() {
        const page = makePage(1280)
        const greeting = find(page, "welcomePage.greeting")
        compare(greeting.font.pixelSize, 40, "as it was")
        compare(greeting.lineCount, 1)
        const discover = find(page, "welcomePage.discoverApplications")
        const manage = find(page, "welcomePage.managePackages")
        compare(at(manage, page).y, at(discover, page).y, "side by side")
        compare(at(manage, page).x - (at(discover, page).x + discover.width), 24, "24 apart")
        const recent = find(page, "welcomePage.recentApps")
        compare(recent.children.length - 1, 5, "five recent apps")
        for (let i = 1; i < 5; ++i)
            compare(recent.children[i].y, recent.children[0].y, "in one row")
        compare(at(recent.children[0], page).x, at(discover, page).x, "the first tile from the column's start")
        compare(discover.height, 136, "the cards as they were")
        launcherTiles(page)
        verify(find(page, "welcomePage.search").shortcutItem.visible, "Ctrl+K, with a keyboard")
    }

    // The recent apps are a launcher's tiles at every width, as Alisher's New
    // tab page has them: 80 px, their names in secondaryText.
    function launcherTiles(page) {
        const wallet = find(page, "welcomePage.recent.wallet_ui")
        compare(wallet.tileSize, 80)
        compare(findChild(wallet, "appGridDelegate.label").font.pixelSize, Theme.typography.secondaryText)
    }

    // ── Phone ──

    // The greeting takes a phone's title size, on one line with its badge;
    // the cards are one above the other.
    function test_phone() {
        const page = makePage(390)
        const greeting = find(page, "welcomePage.greeting")
        compare(greeting.font.pixelSize, 28, "a phone's display size")
        compare(greeting.lineCount, 1)
        const discover = find(page, "welcomePage.discoverApplications")
        const manage = find(page, "welcomePage.managePackages")
        compare(at(manage, page).x, at(discover, page).x, "one above the other")
        verify(at(manage, page).y > at(discover, page).y)
        compare(at(discover, page).x, Theme.pageMargin(390), "the page's margin")
        compare(discover.width, 390 - 2 * Theme.pageMargin(390))
        compare(discover.height, Math.round(136 * Theme.typography.panelTitleText / 24),
                "the cards step down with the titles")
        verify(discover.height < 136)
        verify(at(greeting, page).y <= Theme.spacing.xlarge + 0.5, "near the top, at " + at(greeting, page).y)
        launcherTiles(page)
        const hint = find(page, "welcomePage.search").shortcutItem
        verify(hint.visible, "Ctrl+K in a desktop window as narrow as a phone, which has a keyboard")
        Theme.hasKeyboard = false
        verify(!hint.visible, "none on a phone, which has no keyboard")
    }

    // Recently Closed wraps rather than run off the side.
    function test_phone_recently_closed_wraps() {
        const page = makePage(390)
        const recent = find(page, "welcomePage.recentApps")
        const tiles = []
        for (let i = 0; i < recent.children.length; ++i)
            if (recent.children[i].objectName.startsWith("welcomePage.recent."))
                tiles.push(recent.children[i])
        compare(tiles.length, 5)
        verify(tiles[tiles.length - 1].y > tiles[0].y, "on more than one row")
        for (const t of tiles)
            verify(at(t, page).x + t.width <= 390, t.objectName + " inside")
        compare(at(tiles[0], page).x, Theme.pageMargin(390), "the first tile at the page's margin")
    }
}
