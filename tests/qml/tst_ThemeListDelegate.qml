import QtQuick
import QtTest

// Directory import: see tst_IntentChooserDialog.qml.
import "../../src/Basecamp/Settings"

// One row of Settings → Appearance on its own: it shows what it is given and
// only emits.
TestCase {
    id: testCase
    name: "ThemeListDelegate"
    when: windowShown
    width: 640
    height: 200

    Component {
        id: rowComp
        ThemeListDelegate {
            width: testCase.width
            themeId: "me.ocean"
            name: "Ocean"
            author: "me"
            background: "#0b1e2d"
            accent: "#14786e"
        }
    }

    Component {
        id: spyComp
        SignalSpy {}
    }

    function makeRow(props) {
        const row = createTemporaryObject(rowComp, testCase.Window.window.contentItem, props || {})
        verify(row)
        waitForRendering(row)
        return row
    }

    function test_shows_name_author_and_colours() {
        const row = makeRow()
        compare(findChild(row, "appearance.name.me.ocean").text, "Ocean")
        compare(findChild(row, "appearance.author.me.ocean").text, "by me")
        const stops = findChild(row, "appearance.swatch.me.ocean").gradient.stops
        compare(stops[0].color.toString(), "#0b1e2d")
        compare(stops[3].color.toString(), "#14786e")
    }

    // The radio has no visible label, so it needs a name for screen readers,
    // and a click must not tick it before the host has switched.
    function test_radio_is_named_and_only_asks() {
        const row = makeRow()
        const radio = findChild(row, "appearance.radio.me.ocean")
        compare(radio.Accessible.name, "Ocean")
        mouseClick(radio)
        verify(!radio.checked)
    }

    function test_built_in_has_a_badge_and_no_delete() {
        const row = makeRow({ builtin: true })
        verify(findChild(row, "appearance.builtin.me.ocean").visible)
        verify(!findChild(row, "appearance.remove.me.ocean").visible)
    }

    function test_clicking_asks_to_select_unless_current() {
        const row = makeRow()
        const spy = createTemporaryObject(spyComp, testCase, { target: row, signalName: "selectRequested" })
        mouseClick(row)
        compare(spy.count, 1)
        row.current = true
        mouseClick(row)
        compare(spy.count, 1)
        verify(findChild(row, "appearance.radio.me.ocean").checked)
    }

    function test_delete_asks_to_remove() {
        const row = makeRow()
        const spy = createTemporaryObject(spyComp, testCase, { target: row, signalName: "removeRequested" })
        findChild(row, "appearance.remove.me.ocean").clicked()
        compare(spy.count, 1)
    }
}
