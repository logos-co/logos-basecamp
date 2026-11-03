import QtQuick
import QtTest

import Logos.Controls
import Logos.Theme

// Directory import: see tst_IntentChooserDialog.qml.
import "../../src/Basecamp/Settings"

// Settings → Appearance. main.cpp gives the theme store fixtures/themes;
// install results are fed in the way ContentViews does after the installer runs.
TestCase {
    id: testCase
    name: "AppearanceView"
    when: windowShown
    width: 800
    height: 600

    Component {
        id: viewComp
        AppearanceView {
            width: testCase.width
            height: testCase.height
        }
    }

    Component {
        id: spyComp
        SignalSpy {}
    }

    // themeManager is the host's (main.cpp), as in Basecamp.
    function init() { themeManager.select("dark") }
    function cleanupTestCase() { themeManager.select("dark") }

    // In the window, not the (invisible) TestCase, so `visible` means something.
    function makeView() {
        const view = createTemporaryObject(viewComp, testCase.Window.window.contentItem)
        verify(view)
        // The rows are only in place once laid out, which clicks depend on.
        waitForRendering(view)
        return view
    }

    function row(view, id) { return findChild(view, "appearance.row." + id) }

    function test_lists_dark_first_then_installed_themes() {
        const view = makeView()
        verify(row(view, "dark"))
        verify(row(view, "test.percent"))
        verify(row(view, "test.sample"))
        verify(row(view, "dark").y < row(view, "test.sample").y, "Dark comes first")
        verify(findChild(view, "appearance.builtin.dark").visible)
        verify(!findChild(view, "appearance.builtin.test.sample").visible)
        verify(findChild(view, "appearance.radio.dark").checked)
        verify(!findChild(view, "appearance.radio.test.sample").checked)
    }

    // A short name like "Dark" was cut to "Da…" when its width was capped at
    // its own (fractional) text width. The author goes on its own line.
    function test_name_is_never_cut_and_author_is_below_it() {
        const view = makeView()
        const ids = ["dark", "test.percent", "test.sample"]
        for (let i = 0; i < ids.length; ++i) {
            const name = findChild(view, "appearance.name." + ids[i])
            const author = findChild(view, "appearance.author." + ids[i])
            verify(!name.truncated, ids[i] + " name is cut")
            verify(!author.truncated, ids[i] + " author is cut")
            const nameBottom = name.mapToItem(view, 0, name.height).y
            verify(author.mapToItem(view, 0, 0).y >= nameBottom, ids[i] + " author not below name")
        }
        compare(findChild(view, "appearance.author.dark").text, "by Logos")
    }

    // The tile shows a theme's own background | accent, Dark's where it sets
    // none (test.sample sets a background but no primary). Checks what is
    // painted: the gradient's two halves.
    function test_swatch_shows_each_themes_colours() {
        const view = makeView()
        const dark = findChild(view, "appearance.swatch.dark").gradient.stops
        compare(dark[0].color, Theme.darkTheme.background)
        compare(dark[3].color, Theme.darkTheme.primary)
        const sample = findChild(view, "appearance.swatch.test.sample").gradient.stops
        compare(sample[0].color.toString(), "#fafafa")
        compare(sample[3].color, Theme.darkTheme.primary)
        // Previews are per row, whichever theme is current.
        verify(themeManager.select("test.sample"))
        compare(findChild(view, "appearance.swatch.dark").gradient.stops[0].color,
                Theme.darkTheme.background)
    }

    // Radio, tile and text are spaced evenly, and the tile is as tall as the
    // two text lines.
    function test_row_spacing_is_even() {
        const view = makeView()
        const radio = findChild(view, "appearance.radio.test.sample")
        const tile = findChild(view, "appearance.swatch.test.sample")
        const name = findChild(view, "appearance.name.test.sample")
        const author = findChild(view, "appearance.author.test.sample")
        const radioRight = radio.mapToItem(view, radio.width, 0).x
        const tileLeft = tile.mapToItem(view, 0, 0).x
        const tileRight = tile.mapToItem(view, tile.width, 0).x
        const textLeft = name.mapToItem(view, 0, 0).x
        fuzzyCompare(tileLeft - radioRight, textLeft - tileRight, 1)
        const textHeight = author.mapToItem(view, 0, author.height).y - name.mapToItem(view, 0, 0).y
        verify(Math.abs(tile.height - textHeight) <= 4,
               "tile " + tile.height + " px vs text " + textHeight + " px")
    }

    // Shown as typed: no "%1"/"%2" filling anywhere near a theme's name.
    function test_name_is_shown_as_typed() {
        compare(findChild(makeView(), "appearance.name.test.percent").text, "Odd %2 name")
    }

    // The view only asks; ContentViews switches through themeManager.
    function test_picking_a_row_asks_to_switch() {
        const view = makeView()
        const spy = createTemporaryObject(spyComp, testCase,
                                          { target: view, signalName: "selectRequested" })
        mouseClick(row(view, "test.sample"))
        compare(spy.count, 1)
        compare(spy.signalArguments[0][0], "test.sample")
        compare(Theme.currentTheme, "dark")
    }

    function test_picking_the_radio_asks_to_switch() {
        const view = makeView()
        const spy = createTemporaryObject(spyComp, testCase,
                                          { target: view, signalName: "selectRequested" })
        const radio = findChild(view, "appearance.radio.test.sample")
        mouseClick(radio)
        compare(spy.count, 1)
        compare(spy.signalArguments[0][0], "test.sample")
        // Until the host switches, the radio still shows the real current theme.
        verify(!radio.checked)
    }

    function test_rows_follow_the_current_theme() {
        const view = makeView()
        verify(themeManager.select("test.sample"))
        verify(findChild(view, "appearance.radio.test.sample").checked)
        verify(!findChild(view, "appearance.radio.dark").checked)
        compare(row(view, "test.sample").border.color, Theme.palette.primary)
    }

    // A clean install says its piece and goes: a toast, not a standing notice.
    function test_success_shows_a_toast() {
        const view = makeView()
        const notice = findChild(view, "appearance.resultNotice")
        const toast = findChild(view, "appearance.toast")
        view.showInstallResult({ ok: true, id: "test.sample", name: "Sample",
                                 errors: [], warnings: [] })
        verify(toast.shown)
        compare(toast.title, "Added “Sample”")
        verify(!notice.shown)
    }

    // LogosNotice keeps an empty line for an empty message; every result here
    // must say something under its title.
    function test_results_never_leave_an_empty_message() {
        const view = makeView()
        const notice = findChild(view, "appearance.resultNotice")
        const toast = findChild(view, "appearance.toast")
        const results = [
            () => view.showInstallResult({ ok: true, id: "test.sample", name: "Sample",
                                           errors: [], warnings: [] }),
            () => view.showInstallResult({ ok: true, id: "test.sample", name: "Sample",
                                           errors: [], warnings: ["w"] }),
            () => view.showInstallResult({ ok: false, errors: ["e"], warnings: [] }),
            () => view.showRemoveResult({ ok: true, id: "test.sample", name: "Sample", errors: [] }),
            () => view.showRemoveResult({ ok: false, id: "x", errors: ["e"] }),
        ]
        for (let i = 0; i < results.length; ++i) {
            toast.shown = false
            results[i]()
            const shown = toast.shown ? toast : notice
            verify(shown.shown, "result " + i + " showed nothing")
            verify(shown.message.length > 0, "result " + i + " has an empty message")
        }
    }

    function test_remove_is_offered_only_for_added_themes() {
        const view = makeView()
        verify(!findChild(view, "appearance.remove.dark").visible, "Dark is built in")
        const remove = findChild(view, "appearance.remove.test.sample")
        verify(remove.visible)
        // An icon alone says nothing to a screen reader.
        compare(remove.Accessible.name, "Remove Sample")
    }

    // Two-line rows, so keep them compact: a long list should still fit.
    function test_rows_stay_compact() {
        verify(row(makeView(), "test.sample").height <= 56,
               "row is " + row(makeView(), "test.sample").height + " px")
    }

    // Any added theme can be removed without switching to it first.
    function test_remove_asks_before_removing_any_row() {
        const view = makeView()
        const spy = createTemporaryObject(spyComp, testCase,
                                          { target: view, signalName: "removeRequested" })
        const dialog = findChild(view, "appearance.removeDialog")
        findChild(view, "appearance.remove.test.sample").clicked()
        verify(dialog.opened)
        compare(dialog.title, "Remove “Sample”?")
        compare(dialog.message, "You can add it again from its file.")
        compare(spy.count, 0)
        dialog.rightActions[0].clicked()
        compare(spy.count, 1)
        compare(spy.signalArguments[0][0], "test.sample")
        compare(Theme.currentTheme, "dark")
        verify(!dialog.visible)
        view.showRemoveResult({ ok: true, id: "test.sample", name: "Sample", errors: [] })
        compare(findChild(view, "appearance.toast").message, "It's no longer in your themes.")
    }

    function test_removing_the_theme_in_use_says_it_switches_to_dark() {
        verify(themeManager.select("test.sample"))
        const view = makeView()
        const dialog = findChild(view, "appearance.removeDialog")
        findChild(view, "appearance.remove.test.sample").clicked()
        verify(dialog.message.indexOf("switches back to Dark") >= 0)
        dialog.rightActions[0].clicked()
        view.showRemoveResult({ ok: true, id: "test.sample", name: "Sample", errors: [] })
        compare(findChild(view, "appearance.toast").message, "Switched back to Dark.")
    }

    function test_remove_failure_stays_on_the_page() {
        const view = makeView()
        view.showRemoveResult({ ok: false, id: "x", errors: ["Can't remove /x"] })
        const notice = findChild(view, "appearance.resultNotice")
        verify(notice.shown)
        compare(notice.severity, LogosNotice.Error)
    }

    function test_warnings_are_shown() {
        const view = makeView()
        view.showInstallResult({ ok: true, id: "test.sample", name: "Sample",
                                 errors: [], warnings: ["'radius' isn't supported yet; ignored"] })
        const notice = findChild(view, "appearance.resultNotice")
        compare(notice.severity, LogosNotice.Warning)
        verify(notice.message.indexOf("radius") >= 0)
    }

    function test_failure_keeps_the_current_theme() {
        const view = makeView()
        view.showInstallResult({ ok: false, errors: ["invalid JSON: unterminated object"],
                                 warnings: [] })
        compare(Theme.currentTheme, "dark")
        const notice = findChild(view, "appearance.resultNotice")
        verify(notice.shown)
        compare(notice.severity, LogosNotice.Error)
    }

    // Errors quote key names straight from the uploaded file.
    function test_result_text_is_plain() {
        const view = makeView()
        view.showInstallResult({ ok: false, errors: ["unknown key '<img src=x>' ignored"],
                                 warnings: [] })
        const notice = findChild(view, "appearance.resultNotice")
        compare(notice.messageItem.textFormat, Text.PlainText)
        compare(notice.titleItem.textFormat, Text.PlainText)
    }

    function test_file_picker_is_non_modal() {
        const dialog = findChild(makeView(), "appearance.fileDialog")
        verify(dialog)
        compare(dialog.modality, Qt.NonModal)
    }

    function test_add_button_is_there() {
        const button = findChild(makeView(), "appearance.addButton")
        verify(button)
        verify(button.enabled)
    }
}
