import QtQuick
import QtTest

import Logos.Theme
import Basecamp.AppManager
import Basecamp.Backend
import BasecampTests

import "FitChecks.js" as FitChecks

// The App Manager on the design system, from a phone to a desktop, as
// Alisher's ADR-063 has it: the categories in a sidebar while the apps keep
// their width, a picker in the apps' toolbar otherwise; the toolbar's actions
// folding into ⋯; the search a 🔍 on a phone; tiles three across a phone.
// Nothing checks a width class to get there: the design system's layouts
// measure.
TestCase {
    id: testCase
    name: "AppManagerLayout"
    width: 1280
    height: 800
    // Input can't reach anything in an invisible TestCase, the default.
    visible: true
    when: windowShown

    readonly property string repo: "https://example.test/logos-repo.json"

    function app(name, displayName, category) {
        return {
            name: name, repositoryUrl: repo, displayName: displayName,
            description: displayName + " for Logos.", category: category, type: "ui_qml",
            versions: [{ version: "1.0.0" }],
        }
    }

    TestAppsModel { id: apps }
    TestAppsModel { id: noApps }

    AppsFilterProxy {
        id: uiApps
        sourceModel: apps
        typeFilter: "ui_qml"
        excludeMainUi: true
    }

    AppsFilterProxy {
        id: noUiApps
        sourceModel: noApps
        typeFilter: "ui_qml"
        excludeMainUi: true
    }

    Component {
        id: viewComp
        AppManagerView {
            appsProxy: uiApps
            repositories: [{ url: testCase.repo, displayName: "Logos Apps", enabled: true }]
        }
    }

    Component { id: spyComp; SignalSpy {} }

    function initTestCase() {
        apps.setCatalog([
            app("wallet_ui", "Wallet", "Wallets"),
            app("chat_ui", "Chat", "Chat"),
            app("storage_ui", "Storage", "Storage"),
            app("explorer_ui", "Explorer", "Developer tools"),
            app("notes_ui", "Notes", "Productivity"),
            app("calendar_ui", "Calendar", "Productivity"),
            app("mail_ui", "Mail", "Chat"),
            app("swap_ui", "Swap", "Wallets"),
        ])
        apps.install("wallet_ui", "1.0.0", "user")
        apps.install("chat_ui", "1.0.0", "embedded")
    }

    function init() {
        failOnWarning(/Binding loop/)
    }

    function cleanup() {
        Theme.viewWidth = 0
        uiApps.categoryFilter = "All"
        uiApps.searchText = ""
    }

    // Told its width as a host tells its view: a phone-wide view gets the
    // phone's type and touch sizes.
    function makeView(w, properties) {
        Theme.viewWidth = w
        const view = createTemporaryObject(viewComp, testCase,
                                           Object.assign({ width: w, height: testCase.height }, properties || {}))
        verify(view)
        waitForRendering(view)
        return view
    }

    function noneOverTheSides(view) {
        const over = FitChecks.overflow(view)
        compare(over.map(item => FitChecks.describe(item, view)).join("; "), "", "past the sides")
    }

    function find(view, name) {
        const item = findChild(view, name)
        verify(item, name)
        return item
    }

    function spy(target, signalName) {
        return createTemporaryObject(spyComp, testCase, { target: target, signalName: signalName })
    }

    function openMore(view) {
        const actions = find(view, "appManager.toolbarActions")
        verify(actions.folded)
        mouseClick(actions.moreButtonItem)
        tryCompare(actions.menu, "opened", true)
        // Until laid out, the menu's items sit on top of each other.
        waitForRendering(testCase)
        return actions.menu
    }

    // The app's tile or row, laid out.
    function appItem(view, name) {
        let item = null
        tryVerify(() => (item = findChild(view, "appManager.app." + name)) !== null && item.width > 0, 2000, name)
        return item
    }

    // ── Nothing past the sides ──

    function test_fits_data() {
        const out = []
        for (const w of [360, 390, 600, 700, 900, 1280])
            for (const mode of ["grid", "list"])
                out.push({ tag: w + " " + mode, width: w, mode: mode })
        return out
    }

    function test_fits(data) {
        const view = makeView(data.width)
        if (data.mode === "list")
            find(view, "appManager.panelHeader").viewModeChangeRequested("list")
        waitForRendering(view)
        noneOverTheSides(view)
    }

    // No repositories, and a search that finds nothing, fit too.
    function test_empty_states_fit_data() {
        return [360, 390, 700].map(w => ({ tag: String(w), width: w }))
    }

    function test_empty_states_fit(data) {
        let view = makeView(data.width, { appsProxy: noUiApps, repositories: [] })
        verify(find(view, "appManager.noReposView").visible)
        noneOverTheSides(view)

        view = makeView(data.width)
        view.applySearch("nothing like this")
        waitForRendering(view)
        verify(find(view, "appManager.emptyView").visible)
        noneOverTheSides(view)
    }

    // ── Desktop ──

    // The categories in the sidebar, the toolbar's actions as buttons, the
    // search a field beside the title.
    function test_desktop() {
        const view = makeView(1280)
        verify(find(view, "appManager.body").sideShown)
        verify(find(view, "appManager.category.All").visible)
        verify(!find(view, "appManager.categoryCombo").visible)
        verify(!find(view, "appManager.toolbarActions").folded)
        verify(find(view, "appManager.reloadButton").enabled)
        verify(!find(view, "appManager.searchField").folds)
    }

    // ── Narrower ──

    // No sidebar: the categories are a picker in the toolbar, and its
    // actions fold into ⋯ (ADR-063).
    function test_narrower_a_picker_and_more() {
        const view = makeView(700)
        verify(!find(view, "appManager.body").sideShown)
        const combo = find(view, "appManager.categoryCombo")
        verify(combo.visible)
        verify(find(view, "appManager.toolbarActions").folded)
        verify(!find(view, "appManager.reloadButton").enabled, "the folded buttons are out of reach")
        compare(combo.displayText, "All categories")
        combo.activated(combo.model.indexOf("Wallets"))
        compare(uiApps.categoryFilter, "Wallets")
        compare(combo.displayText, "Wallets")
    }

    function textsIn(item) {
        let out = []
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (child.visible && typeof child.text === "string")
                out.push(child.text)
            out = out.concat(textsIn(child))
        }
        return out
    }

    // The apps panel's header is its tabs, with no "Apps" heading before
    // them: the page's title already says what they list.
    function test_the_panel_header_starts_with_its_tabs_data() {
        return [{ tag: "phone", width: 390 }, { tag: "desktop", width: 1280 }]
    }

    function test_the_panel_header_starts_with_its_tabs(data) {
        const view = makeView(data.width)
        const header = find(view, "appManager.panelHeader")
        const tabs = find(view, "appManager.stateTabs")
        compare(tabs.mapToItem(header, 0, 0).x, 0)
        compare(tabs.width, tabs.implicitWidth, "the tabs at their own width")
        verify(!textsIn(header).includes("Apps"), "no heading")
    }

    // ⋯ holds what the toolbar did: the filter, the grid or list choice,
    // Reload, Manage Repositories.
    function test_more_does_what_the_toolbar_did() {
        const view = makeView(700)
        const header = find(view, "appManager.panelHeader")

        let menu = openMore(view)
        mouseClick(findChild(menu, "appManager.moreShowUnavailableItem"))
        verify(view.showUnavailable)

        menu = openMore(view)
        verify(findChild(menu, "appManager.moreShowUnavailableItem").checked)
        verify(findChild(menu, "appManager.moreGridItem").checked)
        mouseClick(findChild(menu, "appManager.moreListItem"))
        compare(header.viewMode, "list")

        menu = openMore(view)
        verify(findChild(menu, "appManager.moreListItem").checked)
        mouseClick(findChild(menu, "appManager.moreListItem"))
        compare(header.viewMode, "list", "picking the one shown keeps it")
        menu = openMore(view)
        verify(findChild(menu, "appManager.moreListItem").checked, "and its check")

        const reloads = spy(view, "refreshRequested")
        mouseClick(findChild(menu, "appManager.moreReloadItem"))
        compare(reloads.count, 1)

        const repositories = spy(view, "navigateToRepositories")
        menu = openMore(view)
        mouseClick(findChild(menu, "appManager.moreRepositoriesItem"))
        compare(repositories.count, 1)
    }

    // ── Phone ──

    // Three tiles across, from the design system's tile width.
    function test_phone_tiles_three_across() {
        const view = makeView(390)
        const tile = appItem(view, "wallet_ui")
        const grid = tile.parent.parent
        compare(Math.floor(grid.width / tile.width), 3)
    }

    // The search is a 🔍 at the end of the title's line, not a row of its
    // own; open, it filters the apps.
    function test_phone_search_is_on_the_title_line() {
        const view = makeView(390)
        const search = find(view, "appManager.searchField")
        verify(search.folds && !search.expanded)
        const title = find(view, "appManager.header").titleItem
        fuzzyCompare(search.mapToItem(view, 0, 0).y + search.height / 2,
                     title.mapToItem(view, 0, 0).y + title.height / 2, 1, "on the title's line")
        mouseClick(search.openButtonItem)
        tryVerify(() => search.searchBarItem.textInput.activeFocus)
        for (const c of "wal")
            keyClick(c)
        compare(uiApps.searchText, "wal")
    }

    // The welcome page hands its search over: the field shows it, open on a
    // phone, and it filters.
    function test_a_search_handed_over_shows_and_filters() {
        const view = makeView(390)
        view.applySearch("chat")
        const search = find(view, "appManager.searchField")
        compare(search.text, "chat")
        verify(search.expanded)
        compare(uiApps.searchText, "chat")
    }

    // A long press, the touch path, opens an app's menu under its tile, and
    // is not also a tap.
    function test_a_long_press_opens_an_apps_menu() {
        const view = makeView(390)
        const launched = spy(view, "appClicked")
        const tile = appItem(view, "wallet_ui")
        const menu = findChild(tile, "appContextMenu")
        mousePress(tile, tile.width / 2, tile.height / 3)
        tryCompare(menu, "opened", true, 3000)
        mouseRelease(tile, tile.width / 2, tile.height / 3)
        compare(menu.appData.name, "wallet_ui")
        compare(launched.count, 0, "a long press is not a tap")
        menu.close()
        tryCompare(menu, "visible", false)
    }

    function test_a_long_press_on_a_list_row_opens_its_menu() {
        const view = makeView(390)
        find(view, "appManager.panelHeader").viewModeChangeRequested("list")
        // The grid's tiles go once deleted.
        wait(0)
        const row = appItem(view, "chat_ui")
        const menu = findChild(row, "appContextMenu")
        mousePress(row, row.width / 3, row.height / 2)
        tryCompare(menu, "opened", true, 3000)
        mouseRelease(row, row.width / 3, row.height / 2)
        compare(menu.appData.name, "chat_ui")
        menu.close()
        tryCompare(menu, "visible", false)
    }

    // On a phone the page is "Applications" alone: the subtitle would only
    // say it again. Wider, both.
    function test_the_subtitle_is_off_on_a_phone_data() {
        return [{ tag: "phone", width: 390, phone: true }, { tag: "desktop", width: 1280, phone: false }]
    }

    function test_the_subtitle_is_off_on_a_phone(data) {
        const view = makeView(data.width)
        const header = find(view, "appManager.header")
        compare(header.titleItem.text, "Applications")
        compare(header.subtitleItem.visible, !data.phone)
    }

    // Where nothing hovers, a list row's menu is behind a visible ⋯, in place
    // of the trash, and a row's install badge shows without a hover.
    function test_on_touch_a_list_row_shows_its_menu_and_badge() {
        const view = makeView(390)
        find(view, "appManager.panelHeader").viewModeChangeRequested("list")
        wait(0)
        const row = appItem(view, "wallet_ui")
        const more = findChild(row, "appListDelegate.more")
        verify(more.visible, "a ⋯")
        verify(more.width >= Theme.size.touchTarget && more.height >= Theme.size.touchTarget, "a fingertip")
        verify(!findChild(row, "appListDelegate.uninstall").visible, "no trash beside it")
        const launched = spy(view, "appClicked")
        const menu = findChild(row, "appContextMenu")
        mouseClick(more)
        tryCompare(menu, "opened", true)
        compare(menu.appData.name, "wallet_ui")
        compare(launched.count, 0, "not also a tap on the row")
        menu.close()
        tryCompare(menu, "visible", false)
        verify(findChild(appItem(view, "storage_ui"), "appListStateBadge").visible, "Install, unhovered")
    }

    // On a desktop the row keeps its trash and a right-click; no ⋯.
    function test_a_desktop_list_row_keeps_its_trash() {
        const view = makeView(1280)
        find(view, "appManager.panelHeader").viewModeChangeRequested("list")
        wait(0)
        const row = appItem(view, "wallet_ui")
        verify(!findChild(row, "appListDelegate.more").visible)
        verify(findChild(row, "appListDelegate.uninstall").visible)
    }

    // A right-click still opens it, where it was clicked.
    function test_a_right_click_opens_an_apps_menu() {
        const view = makeView(1280)
        const tile = appItem(view, "wallet_ui")
        const menu = findChild(tile, "appContextMenu")
        mouseClick(tile, tile.width / 2, tile.height / 3, Qt.RightButton)
        tryCompare(menu, "opened", true)
        verify(!menu.sheet)
        compare(menu.appData.name, "wallet_ui")
        menu.close()
        tryCompare(menu, "visible", false)
    }
}
