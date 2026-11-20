import QtQuick
import QtTest

import Logos.Controls
import Logos.Theme

// Directory import: see tst_IntentChooserDialog.qml.
import "../../src/Basecamp/Settings"

// The inspectors on a phone: each row is a card, its Load/Unload and
// Interface behind the card's ⋯, with the rules ModuleRowActions has beside
// the rows on a wider pane.
TestCase {
    id: testCase
    name: "InspectorRowMenu"
    when: windowShown
    width: 900
    height: 600

    ListModel {
        id: modules
        ListElement { name: "depsvc"; label: "depsvc"; statusText: "Loaded"; isLoaded: true
                      isMainUi: false; hasMissingDeps: false; cpu: 1.5; memory: 12.0; version: ""; description: "" }
        ListElement { name: "package_manager"; label: "package_manager"; statusText: "Loaded"; isLoaded: true
                      isMainUi: false; hasMissingDeps: false; cpu: 0.5; memory: 30.0; version: ""; description: "" }
        ListElement { name: "idle"; label: "idle"; statusText: "Not loaded"; isLoaded: false
                      isMainUi: false; hasMissingDeps: false; cpu: 0; memory: 0; version: "1.0.0"; description: "" }
    }

    Component { id: moduleInspectorComp; ModuleInspectorView { sourceModel: modules } }
    Component { id: appsInspectorComp; AppsInspectorView { sourceModel: modules } }

    SignalSpy { id: loadSpy; signalName: "loadRequested" }
    SignalSpy { id: unloadSpy; signalName: "unloadRequested" }

    function makeView(comp, w) {
        const view = createTemporaryObject(comp, testCase.Window.window.contentItem,
                                           { width: w, height: testCase.height })
        verify(view)
        loadSpy.clear(); loadSpy.target = view
        unloadSpy.clear(); unloadSpy.target = view
        waitForRendering(view)
        return view
    }

    function tableOf(view) {
        return findChild(view, view.objectName === "moduleInspectorView" ? "moduleInspector.table"
                                                                         : "appsInspector.table")
    }

    // The menu ⋯ opens on the card of `name`.
    function openMenuFor(view, name) {
        const table = tableOf(view)
        const cards = table.cardsView
        let button = null
        tryVerify(() => {
            for (let i = 0; i < cards.count; ++i) {
                const card = cards.itemAtIndex(i) ? cards.itemAtIndex(i).item : null
                if (card && card.capturedRowItem && card.capturedRowItem.name === name)
                    button = findChild(card.trailing, "cardMenuButton")
            }
            return button !== null
        }, 2000, "a card with ⋯ for " + name)
        mouseClick(button)
        const menu = table.rowMenu
        tryVerify(() => menu.opened, 2000, "the row menu opens")
        compare(table.menuRow.name, name)
        return menu
    }

    function item(menu, name) {
        for (let i = 0; i < menu.count; ++i)
            if (menu.itemAt(i).objectName === name)
                return menu.itemAt(i)
        return null
    }

    function test_phone_rows_are_cards_with_their_actions_behind_a_menu() {
        const view = makeView(moduleInspectorComp, 390)
        const table = tableOf(view)
        verify(table.showingCards)
        verify(table.rowMenu)

        const menu = openMenuFor(view, "depsvc")
        const toggle = item(menu, "moduleRowMenu.loadToggle")
        compare(toggle.text, "Unload")
        verify(toggle.enabled)
        toggle.triggered()
        compare(unloadSpy.count, 1)
        compare(unloadSpy.signalArguments[0][0], "depsvc")
    }

    function test_phone_menu_opens_a_loaded_modules_interface() {
        const view = makeView(moduleInspectorComp, 390)
        const menu = openMenuFor(view, "depsvc")
        const iface = item(menu, "moduleRowMenu.interface")
        verify(iface.visible)
        iface.triggered()
        verify(view.showingInterface)
        compare(view.selectedPlugin, "depsvc")
    }

    // A card's ⋯ opens its detail too, as a tap on it does.
    function test_phone_menu_opens_the_details() {
        const view = makeView(appsInspectorComp, 390)
        const menu = openMenuFor(view, "idle")
        item(menu, "moduleRowMenu.open").triggered()
        verify(findChild(view, "appsInspector.details").panelOpen)
        compare(findChild(view, "appsInspector.details").title, "idle")
    }

    function test_phone_menu_keeps_the_protected_modules_locked() {
        const view = makeView(moduleInspectorComp, 390)
        const menu = openMenuFor(view, "package_manager")
        verify(!item(menu, "moduleRowMenu.loadToggle").enabled)
    }

    function test_phone_menu_loads_an_app_and_has_no_interface() {
        const view = makeView(appsInspectorComp, 390)
        const menu = openMenuFor(view, "idle")
        const toggle = item(menu, "moduleRowMenu.loadToggle")
        compare(toggle.text, "Load")
        verify(!item(menu, "moduleRowMenu.interface").visible)
        compare(item(menu, "moduleRowMenu.interface").height, 0)
        toggle.triggered()
        compare(loadSpy.count, 1)
        compare(loadSpy.signalArguments[0][0], "idle")
    }

    // Wider than a phone the rows keep their buttons, which the UI tests click.
    function test_desktop_keeps_the_buttons_beside_the_rows() {
        const view = makeView(moduleInspectorComp, 900)
        const table = tableOf(view)
        verify(!table.showingCards)
        compare(table.rowMenu, null)
        tryVerify(() => findChild(view, "moduleRow.loadToggle.depsvc") !== null, 2000)
    }
}
