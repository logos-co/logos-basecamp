import QtQuick
import QtTest

import Logos.Controls
import Logos.Theme

// Directory import: see tst_IntentChooserDialog.qml.
import "../../src/Basecamp/Settings"
import "FitChecks.js" as FitChecks

// Settings on the design system's LogosSections, in Basecamp's look: the title,
// subtitle and the inspectors' search over the Sections list beside the open
// section; on a phone the list is the page and a section opens under
// "‹ Settings".
TestCase {
    id: testCase
    name: "SettingsView"
    when: windowShown
    width: 1200
    height: 700

    Component {
        id: viewComp
        SettingsView {}
    }

    // What each section shows, with the long names, URLs and keys that push
    // a narrow page.
    ListModel {
        id: appsModel
        ListElement { name: "package_manager_ui"; label: "Package Manager"; statusText: "Loaded"; isLoaded: true
                      isMainUi: false; hasMissingDeps: false; cpu: 0; memory: 0; version: "1.0.0"
                      description: "Install, update and remove packages." }
        ListElement { name: "wallet_ui"; label: "Wallet"; statusText: "Not loaded"; isLoaded: false
                      isMainUi: false; hasMissingDeps: false; cpu: 0; memory: 0; version: "0.4.2"
                      description: "Send and receive tokens on the Logos blockchain." }
    }
    ListModel {
        id: modulesModel
        ListElement { name: "package_manager"; label: "Package Manager Module"; statusText: "Loaded"; isLoaded: true
                      isMainUi: false; hasMissingDeps: false; cpu: 0.4; memory: 18.2; version: "1.0.0"; description: "" }
        ListElement { name: "storage_module"; label: "Storage"; statusText: "Not loaded"; isLoaded: false
                      isMainUi: false; hasMissingDeps: false; cpu: 0; memory: 0; version: "0.4.1"; description: "" }
    }

    function filled() {
        return {
            uiModulesModel: appsModel,
            coreModulesModel: modulesModel,
            downloadSource: "any",
            buildVersion: "0.2.0-rc3",
            userDirectory: "/Users/someone-with-a-long-name/Library/Application Support/Logos/LogosBasecampDev",
            buildCommits: ["logos-basecamp", "logos-package-manager-module", "nix-bundle-logos-module-install"]
                .map(name => ({ name: name, commit: "9c3ba855aa976516ca6aff2031d8731aec827ad0" })),
            repositories: [
                { url: "https://raw.githubusercontent.com/logos-co/logos-modules-release/main/logos-repo.json",
                  displayName: "Logos", isDefault: true, enabled: true, description: "The official Logos catalog." },
                { url: "https://example.org/community/logos-repo.json", displayName: "Community", enabled: false,
                  resolveError: "Could not resolve host: example.org" },
                { url: "https://mirror.example.net/logos-repo.json", displayName: "Logos", enabled: true }
            ],
            trustedPublishers: [
                { name: "logos", did: "did:jwk:logos", displayName: "Logos", url: "https://logos.co",
                  fingerprint: "4BBC C11D CB35 F9FD", builtIn: true, addedAt: "2026-10-01T10:00:00Z" },
                { name: "publisher-0123456789abcdef", did: "did:jwk:other", displayName: "Other publisher",
                  fingerprint: "AE21 6C2E F524 7A37", url: "https://other.example", builtIn: false,
                  addedAt: "2026-11-02T10:00:00Z" }
            ]
        }
    }

    // Told its width as Basecamp tells its content view's theme (MainContainer's
    // followViewWidth): a phone-wide view gets the phone's type and touch sizes.
    function makeView(w, props) {
        Theme.viewWidth = w
        const view = createTemporaryObject(viewComp, testCase.Window.window.contentItem,
                                           Object.assign({ width: w, height: testCase.height }, props || {}))
        verify(view)
        waitForRendering(view)
        return view
    }

    // A binding loop leaves a property behind what it is bound to: one in
    // LogosItemLayout left a row laid out for the other line.
    function init() { failOnWarning(/Binding loop/) }

    function cleanup() { Theme.viewWidth = 0 }

    // The LogosSections inside: the child with the sections' list.
    function sections(view) {
        for (let i = 0; i < view.children.length; ++i)
            if (view.children[i].listItem !== undefined)
                return view.children[i]
        return null
    }

    // Opens section `index`, on a phone too, where setting the index it is
    // already at would not.
    function open(s, index) {
        s.currentIndex = (index + 1) % s.sections.length
        s.currentIndex = index
    }

    function search(view) { return findChild(view, "settings.searchField") }
    function shown(item) { return !!item && item.visible }

    function test_desktop_lists_the_sections_beside_the_open_one() {
        const view = makeView(1200)
        const s = sections(view)
        verify(s.twoPane)
        compare(s.titleItem.text, "Settings")
        compare(s.subtitleItem.text, "Manage modules, apps and dashboards.")
        compare(s.listItem.headerItem.text, "Sections")
        compare(s.listItem.count, 6)
        verify(!shown(search(view)), "the Dashboard has nothing to search")
    }

    function test_the_inspectors_share_a_search_that_a_switch_clears() {
        const view = makeView(1200)
        const s = sections(view)
        const apps = findChild(view, "appsInspectorView")
        const modules = findChild(view, "moduleInspectorView")

        s.currentIndex = 1
        verify(shown(search(view)))
        search(view).searchBarItem.textInput.forceActiveFocus()
        for (const c of "package")
            keyClick(c)
        compare(apps.searchText, "package")

        s.currentIndex = 2
        compare(search(view).text, "", "a query doesn't follow the user to another section")
        compare(modules.searchText, "")
        compare(search(view).placeholderText, "Search modules…")

        s.currentIndex = 3
        verify(!shown(search(view)))
    }

    // On a phone the search is a 🔍 at the end of the "‹ Settings" row, as
    // Alisher's ADR-058 has it, not a row of its own over the inspector; open,
    // it takes the row and filters the inspector.
    function test_phone_search_is_in_the_back_row() {
        const view = makeView(360, filled())
        const s = sections(view)
        const apps = findChild(view, "appsInspectorView")
        open(s, 1)
        waitForRendering(view)
        const field = search(view), back = s.backRowItem
        verify(shown(field))
        verify(!field.expanded, "an icon")
        fuzzyCompare(field.mapToItem(view, 0, field.height / 2).y, back.mapToItem(view, 0, back.height / 2).y, 1,
                     "on the back row")
        verify(s.sectionItem.mapToItem(view, 0, 0).y < back.mapToItem(view, 0, 0).y + back.height + Theme.spacing.large,
               "the inspector right under the row")

        mouseClick(field.openButtonItem)
        verify(!back.visible, "the search takes the row")
        tryVerify(() => field.searchBarItem.textInput.activeFocus)
        for (const c of "wallet")
            keyClick(c)
        compare(apps.searchText, "wallet")

        waitForRendering(view)
        mouseClick(field.closeButtonItem)
        verify(back.visible)
        compare(apps.searchText, "")
    }

    // On a phone a row's detail is the page: the search, for the table under
    // it, steps aside until it closes.
    function test_phone_detail_page_has_no_search() {
        const view = makeView(360, filled())
        const s = sections(view)
        open(s, 1)
        verify(shown(search(view)))
        const apps = findChild(view, "appsInspectorView")
        apps.openDetails("wallet_ui")
        verify(apps.canGoBack)
        verify(!shown(search(view)))
        findChild(view, "appsInspector.details").panelOpen = false
        verify(shown(search(view)))
    }

    // One way back, as Alisher's ADR-054 has it: on a phone Settings' back row
    // names the level it goes back to and goes up one at a time, and the
    // detail draws no back of its own.
    function test_phone_one_way_back_through_an_inspector() {
        const view = makeView(360, filled())
        const s = sections(view)
        open(s, 1)
        compare(s.backRowItem.label, "Settings")

        findChild(view, "appsInspectorView").openDetails("wallet_ui")
        waitForRendering(view)
        compare(s.backRowItem.label, "Apps Inspector")
        verify(!findChild(view, "appsInspector.details").headerItem.backButtonItem.visible, "no second back")
        verify(!s.sectionBackRowItem.visible)

        mouseClick(s.backRowItem)
        verify(!findChild(view, "appsInspector.details").panelOpen)
        verify(s.showingSection, "back in the inspector, not out of it")
        compare(s.backRowItem.label, "Settings")

        // A turn of the event loop between taps, as between a person's.
        wait(0)
        mouseClick(s.backRowItem)
        verify(!s.showingSection)
    }

    // Beside the list, a module's Interface goes back through the same row,
    // at the top of the section's card, in place of its own "← Back".
    function test_the_interface_goes_back_through_the_row_in_the_card() {
        const view = makeView(1200, filled())
        const s = sections(view)
        open(s, 2)
        const modules = findChild(view, "moduleInspectorView")
        verify(!s.sectionBackRowItem.visible)

        modules.openInterface("package_manager")
        waitForRendering(view)
        verify(s.sectionBackRowItem.visible)
        compare(findChild(view, "settings.sectionBack"), s.sectionBackRowItem)
        compare(s.sectionBackRowItem.label, "Module Inspector")
        verify(!shown(search(view)), "no search over the Interface")

        mouseClick(s.sectionBackRowItem)
        verify(!modules.showingInterface)
        verify(!s.sectionBackRowItem.visible)
        verify(shown(search(view)))
    }

    SignalSpy { id: openUserDirectorySpy; signalName: "openUserDirectoryRequested" }

    // The Dashboard names the user dir in use and opens it; when the file
    // manager won't, it says so, until the next try.
    function test_dashboard_shows_and_opens_the_user_directory() {
        const view = makeView(1200, filled())
        compare(findChild(view, "dashboard.userDirectory").text, filled().userDirectory)
        openUserDirectorySpy.target = view
        openUserDirectorySpy.clear()
        const failed = findChild(view, "dashboard.openUserDirectoryFailed")
        verify(!failed.visible)

        mouseClick(findChild(view, "dashboard.openUserDirectory"))
        compare(openUserDirectorySpy.count, 1)
        view.reportOpenUserDirectoryFailed()
        verify(failed.visible)

        mouseClick(findChild(view, "dashboard.openUserDirectory"))
        compare(openUserDirectorySpy.count, 2)
        verify(!failed.visible)
    }

    function test_dashboard_without_a_user_directory_hides_it() {
        const view = makeView(1200)
        verify(!findChild(view, "dashboard.openUserDirectory").visible)
    }

    function test_show_repositories_opens_its_section() {
        const view = makeView(1200)
        view.showRepositories()
        compare(sections(view).currentIndex, 4)
    }

    function test_without_repositories_their_sections_go() {
        const view = makeView(1200, { repositoriesAvailable: false })
        compare(sections(view).listItem.count, 4)
        view.showRepositories()
        compare(sections(view).currentIndex, 0)
    }

    function test_phone_list_is_the_page_and_back_returns_to_it() {
        const view = makeView(360)
        const s = sections(view)
        verify(!s.twoPane)
        verify(s.phoneListItem.visible)
        verify(!s.sectionItem.visible)

        view.showRepositories()
        verify(s.showingSection)
        verify(s.backRowItem.visible)
        mouseClick(s.backRowItem)
        verify(!s.showingSection)
        verify(s.phoneListItem.visible)
    }

    // ── Every section fits ──

    readonly property var sectionNames: ["Dashboard", "Apps Inspector", "Module Inspector", "Appearance",
                                         "Package Repositories", "Trusted Publishers"]

    // What each section must still show, inside its card.
    readonly property var mustShow: [
        ["dashboard.userDirectory", "dashboard.openUserDirectory"], [], [],
        ["appearance.addButton"],
        ["repositories.refreshButton", "repositories.addButton",
         "repositories.enableSwitch.https://example.org/community/logos-repo.json",
         "repositories.remove.https://example.org/community/logos-repo.json"],
        ["trustedPublishers.remove.logos", "trustedPublishers.copy.publisher-0123456789abcdef",
         "trustedPublishers.remove.publisher-0123456789abcdef"]
    ]

    // At Basecamp's narrowest (a 360 px window leaves Settings 260), in a
    // tablet-wide window, whose card is phone-wide, and on desktops, nothing
    // in a section reaches past its card, which would cut it off, and nothing
    // scrolls sideways.
    function test_sections_fit_data() {
        const rows = []
        for (const width of [260, 700, 1000, 1300])
            for (let i = 0; i < sectionNames.length; ++i)
                rows.push({ tag: width + " " + sectionNames[i], width: width, section: i })
        return rows
    }

    function test_sections_fit(data) {
        const view = makeView(data.width, filled())
        const s = sections(view)
        open(s, data.section)
        waitForRendering(view)
        const card = s.sectionItem
        const found = FitChecks.overflow(card).concat(FitChecks.overflow(view))
        verify(found.length === 0, "past the sides: "
               + found.map(item => FitChecks.describe(item, card)).join("; "))
        for (const name of mustShow[data.section]) {
            const item = findChild(view, name)
            verify(item && item.visible, name + " is shown")
            const left = item.mapToItem(card, 0, 0).x
            verify(left >= 0 && left + item.width <= card.width, name + " is inside the card")
        }
    }

    // A module's Interface, opened from the Module Inspector, fits too: long
    // names and signatures wrap, as does the result.
    function test_interface_fits_data() {
        return [260, 700, 1000].map(width => ({ tag: String(width), width: width }))
    }

    function test_interface_fits(data) {
        const view = makeView(data.width, filled())
        const s = sections(view)
        open(s, 2)
        findChild(view, "moduleInspectorView").openInterface("package_manager")
        const iface = findChild(view, "pluginInterfaceView")
        iface.methods = [{ name: "requestInstallPackageFromRepository",
                           signature: "(QString name, QString version, QString repositoryUrl)",
                           description: "Installs a package and what it depends on." }]
        iface.events = [{ name: "packageInstallProgressChanged", signature: "(QString name, int percent)" }]
        iface.resultText = JSON.stringify({ ok: true, installed: ["package_manager", "storage_module"] })
        waitForRendering(view)
        verify(iface.visible)
        const card = s.sectionItem
        const found = FitChecks.overflow(card)
        verify(found.length === 0, "past the sides: "
               + found.map(item => FitChecks.describe(item, card)).join("; "))
    }

    // An inspector with a row's detail open fits too: beside the table, or as
    // the page on a phone.
    function test_inspector_details_fit_data() {
        const rows = []
        for (const width of [260, 700, 1000, 1300]) {
            rows.push({ tag: width + " Apps Inspector", width: width, section: 1,
                        view: "appsInspectorView", name: "package_manager_ui" })
            rows.push({ tag: width + " Module Inspector", width: width, section: 2,
                        view: "moduleInspectorView", name: "package_manager" })
        }
        return rows
    }

    function test_inspector_details_fit(data) {
        const view = makeView(data.width, filled())
        const s = sections(view)
        open(s, data.section)
        findChild(view, data.view).openDetails(data.name)
        waitForRendering(view)
        const card = s.sectionItem
        const found = FitChecks.overflow(card)
        verify(found.length === 0, "past the sides: "
               + found.map(item => FitChecks.describe(item, card)).join("; "))
    }
}
