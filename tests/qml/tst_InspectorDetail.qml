import QtQuick
import QtTest

import Logos.Controls
import Logos.Theme

// Directory import: see tst_IntentChooserDialog.qml.
import "../../src/Basecamp/Settings"

// The inspectors' detail: a tap on a row opens it beside the table, or as the
// page on a phone, with every field, the columns the table drops among them,
// and the row's actions. On a phone it, and a module's Interface, are levels
// whose one way back is Settings' back row (canGoBack, backTitle, goBack()).
TestCase {
    id: testCase
    name: "InspectorDetail"
    when: windowShown
    width: 1000
    height: 600

    readonly property var rows: [
        { name: "depsvc", label: "Dependency Service", statusText: "Loaded", isLoaded: true,
          isMainUi: false, hasMissingDeps: false, cpu: 1.5, memory: 12.0, version: "2.1.0",
          description: "Resolves what a package depends on." },
        { name: "package_manager", label: "package_manager", statusText: "Loaded", isLoaded: true,
          isMainUi: false, hasMissingDeps: false, cpu: 0.5, memory: 30.0, version: "", description: "" },
        { name: "idle", label: "idle", statusText: "Not loaded", isLoaded: false,
          isMainUi: false, hasMissingDeps: false, cpu: 0, memory: 0, version: "1.0.0", description: "" },
        { name: "main_ui", label: "main_ui", statusText: "Loaded", isLoaded: true,
          isMainUi: true, hasMissingDeps: false, cpu: 0, memory: 0, version: "", description: "" }
    ]

    ListModel { id: modules }
    ListModel { id: modulesAgain }

    Component { id: moduleInspectorComp; ModuleInspectorView { sourceModel: modules } }
    Component { id: appsInspectorComp; AppsInspectorView { sourceModel: modules } }

    SignalSpy { id: loadSpy; signalName: "loadRequested" }
    SignalSpy { id: unloadSpy; signalName: "unloadRequested" }

    // Each test starts from the same rows.
    function init() {
        modules.clear()
        for (const row of rows)
            modules.append(row)
    }

    function makeView(comp, w) {
        const view = createTemporaryObject(comp, testCase.Window.window.contentItem,
                                           { width: w, height: testCase.height })
        verify(view)
        loadSpy.clear(); loadSpy.target = view
        unloadSpy.clear(); unloadSpy.target = view
        waitForRendering(view)
        return view
    }

    function apps(view) { return view.objectName === "appsInspectorView" }
    function tableOf(view) { return findChild(view, apps(view) ? "appsInspector.table" : "moduleInspector.table") }
    function detailsOf(view) { return findChild(view, apps(view) ? "appsInspector.details" : "moduleInspector.details") }
    function titleOf(view) { return detailsOf(view).title }

    // The shown text in `item`'s tree that reads `text` exactly.
    function findText(item, text) {
        if (item.visible && item.text === text)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = findText(item.children[i], text)
            if (found)
                return found
        }
        return null
    }

    // Taps `name`'s row, or its card on a phone, by its title.
    function tapRow(view, name) {
        const table = tableOf(view)
        const list = table.showingCards ? table.cardsView : table.view
        let target = null
        tryVerify(() => {
            for (let i = 0; i < list.count; ++i) {
                const loader = list.itemAtIndex(i)
                const row = loader ? (table.showingCards ? loader.rowItem : loader.item.capturedRowItem) : null
                if (row && row.name === name)
                    target = loader
            }
            return target !== null
        }, 2000, "a row for " + name)
        // On the title, clear of the row's buttons.
        mouseClick(target, Theme.spacing.large * 2, target.height / 2)
        // The panel's buttons are only in place once it is laid out.
        waitForRendering(view)
    }

    function test_a_tap_opens_the_detail_beside_the_table() {
        const view = makeView(appsInspectorComp, 1000)
        const details = detailsOf(view)
        verify(!details.panelOpen)

        tapRow(view, "depsvc")
        verify(details.panelOpen)
        verify(details.beside)
        // Its name and ✕ in the panel's bar.
        compare(details.title, "Dependency Service")
        verify(details.headerItem.visible)
        verify(!view.canGoBack, "beside the table it is no level: ✕ closes it")
        // The table makes room for it, dropping the Description it shows.
        verify(details.contentPaneItem.width < view.width - details.panelWidth + 1)
        verify(findText(details.panelFrameItem, "Resolves what a package depends on."))
        compare(findChild(view, "moduleDetail.subtitle").text, "v2.1.0 · depsvc")

        mouseClick(details.closeButtonItem)
        verify(!details.panelOpen)
    }

    // On a phone the detail is the page, with no back of its own: Settings'
    // back row, named by backTitle, goes back through goBack().
    function test_on_a_phone_a_card_opens_the_detail_as_the_page() {
        const view = makeView(moduleInspectorComp, 390)
        const details = detailsOf(view)
        verify(tableOf(view).showingCards)

        tapRow(view, "depsvc")
        verify(details.showingPanel)
        verify(!details.contentPaneItem.visible, "the detail is the page")
        verify(details.headerItem.visible, "its bar names it")
        verify(!details.headerItem.backButtonItem.visible, "no back of its own")
        compare(titleOf(view), "Dependency Service")
        verify(view.canGoBack)
        compare(view.backTitle, "Module Inspector")
        verify(details.actionBarItem.visible, "its actions are a bar at the bottom")
        verify(findText(details.panelFrameItem, "1.5%"))
        verify(findText(details.panelFrameItem, "12.0 MB"))

        const toggle = findChild(view, "moduleInspector.details.loadToggle")
        compare(toggle.text, "Unload")
        mouseClick(toggle)
        compare(unloadSpy.count, 1)
        compare(unloadSpy.signalArguments[0][0], "depsvc")

        view.goBack()
        verify(!details.panelOpen)
        verify(details.contentPaneItem.visible)
        verify(!view.canGoBack)
    }

    // A module's Interface is a level too, back goes up one at a time: from
    // the Interface to the detail it was opened from, then to the list.
    function test_back_goes_up_one_level_at_a_time() {
        const view = makeView(moduleInspectorComp, 390)
        view.openDetails("depsvc")
        findChild(view, "moduleInspector.details.interface").clicked()
        verify(view.showingInterface)
        compare(view.backTitle, "Dependency Service")

        view.goBack()
        verify(!view.showingInterface)
        verify(detailsOf(view).showingPanel)
        compare(view.backTitle, "Module Inspector")

        view.goBack()
        verify(!detailsOf(view).panelOpen)
        verify(!view.canGoBack)
    }

    // Beside the table an Interface opened from a row goes back to the list.
    function test_beside_the_table_the_interface_goes_back_to_the_list() {
        const view = makeView(moduleInspectorComp, 1000)
        view.openInterface("depsvc")
        verify(view.canGoBack)
        compare(view.backTitle, "Module Inspector")
        view.goBack()
        verify(!view.showingInterface)
        verify(!view.canGoBack)
    }

    // The stats poll updates the open detail, and a search that hides the row
    // leaves it open.
    function test_the_detail_follows_its_module() {
        const view = makeView(moduleInspectorComp, 1000)
        view.openDetails("depsvc")
        const details = detailsOf(view)
        verify(findText(details.panelFrameItem, "1.5%"))

        modules.setProperty(0, "cpu", 2.5)
        tryVerify(() => findText(details.panelFrameItem, "2.5%") !== null, 2000, "the new CPU figure")

        view.searchText = "zzz"
        compare(tableOf(view).model.visibleCount, 0)
        verify(details.panelOpen)
        compare(titleOf(view), "Dependency Service")
    }

    function test_a_module_not_loaded_has_no_stats_or_interface() {
        const view = makeView(moduleInspectorComp, 1000)
        view.openDetails("idle")
        const details = detailsOf(view)
        verify(!findText(details.panelFrameItem, "CPU"), "no CPU for a module that isn't running")
        verify(!findText(details.panelFrameItem, "Memory"))
        verify(!findChild(view, "moduleInspector.details.interface").visible)
        const toggle = findChild(view, "moduleInspector.details.loadToggle")
        compare(toggle.text, "Load")
        toggle.clicked()
        compare(loadSpy.count, 1)
        compare(loadSpy.signalArguments[0][0], "idle")
    }

    // Basecamp's own UI has no Load or Unload, so its detail has no actions,
    // and on a phone no empty bar where they would be.
    function test_basecamp_itself_has_no_action_bar() {
        const view = makeView(appsInspectorComp, 390)
        view.openDetails("main_ui")
        waitForRendering(view)
        const details = detailsOf(view)
        verify(details.showingPanel)
        verify(!findChild(view, "appsInspector.details.loadToggle").visible)
        compare(details.actionBarItem.height, 0)
    }

    function test_the_modules_basecamp_runs_on_stay_loaded() {
        const view = makeView(moduleInspectorComp, 1000)
        view.openDetails("package_manager")
        verify(!findChild(view, "moduleInspector.details.loadToggle").enabled)
    }

    function test_the_interface_opens_from_the_detail() {
        const view = makeView(moduleInspectorComp, 1000)
        view.openDetails("depsvc")
        findChild(view, "moduleInspector.details.interface").clicked()
        verify(view.showingInterface)
        compare(view.selectedPlugin, "depsvc")
    }

    // The modules model resets whenever a module comes or goes, and every row
    // is made again: a row on its way out has no data, and the open detail
    // must not read it. The apps' model too, as an install ends.
    function test_a_reset_under_an_open_detail_reads_no_gone_row_data() {
        return [{ tag: "modules", comp: moduleInspectorComp }, { tag: "apps", comp: appsInspectorComp }]
    }

    function test_a_reset_under_an_open_detail_reads_no_gone_row(data) {
        failOnWarning(/Cannot read property/)
        const view = makeView(data.comp, 1000)
        view.openDetails("depsvc")
        const details = detailsOf(view)
        verify(details.panelOpen)
        modulesAgain.clear()
        for (const row of rows)
            modulesAgain.append(row)
        view.sourceModel = modulesAgain
        waitForRendering(view)
        verify(details.panelOpen)
        compare(titleOf(view), "Dependency Service")
    }

    function test_a_module_that_goes_closes_its_detail() {
        const view = makeView(moduleInspectorComp, 1000)
        view.openDetails("idle")
        const details = detailsOf(view)
        verify(details.panelOpen)
        modules.remove(2)
        tryVerify(() => !details.panelOpen, 2000, "the detail closes")
    }
}
