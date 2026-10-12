import QtQuick
import QtQuick.Controls
import QtTest
import Basecamp.Backend 1.0

// A real module here, not the directory import the Shell tests use: this
// dialog reaches the AppColors and DownloadFormat singletons, which are
// registered by CMake and have no on-disk qmldir to resolve them from. See
// this suite's CMakeLists for the matching qt_add_qml_module.
import Basecamp.AppManager

// The Required Packages list, and what happens when the dialog runs out of
// window.
//
// It used to be sized to its own contentHeight with interactive: false, which
// is two ways of saying the same thing — the list was always exactly as tall as
// its contents, so it never had anything out of view to scroll to, and the
// dialog grew a row taller per dependency. An app with a long dependency chain
// pushed Install, Uninstall and the footer summary off the bottom of the
// window, and there was no way to get to them.
//
// The fix is a capped dialog plus a ColumnLayout in which only the list has any
// slack. That second half is the fragile one: Layout.minimumHeight defaults to
// 0, so a section that forgets to declare one silently squashes instead of the
// list scrolling. test_a_short_window_takes_it_out_of_the_list_only is the
// guard on that.
TestCase {
    id: testCase
    name: "AddApplicationDialog"
    when: windowShown
    width: 900
    height: 1200

    readonly property int rowHeight: 56
    readonly property int maxRows: 6
    readonly property int showcaseHeight: 200

    // Taller than the dialog ever wants to be, so the cap never engages.
    readonly property int roomyWindow: 1100
    // Short enough to force a shrink, with room left for several rows.
    readonly property int crampedWindow: 760

    // Stands in for AppsFilterProxy: the dialog reads a handful of counters off
    // the model and nothing else, so the counters are plain properties here.
    // The rows carry the roles PackageRowDelegate actually touches.
    Component {
        id: modelComp
        ListModel {
            property int visibleCount: count
            property int installedCount: 0
            property bool hasResolutionErrors: false
            property int installFreshCount: count
            property int upgradeCount: 0
            property int reinstallCount: 0
            property int alreadyInstalledCount: 0
            property int installingCount: 0
            property int errorCount: 0
            property real totalDownloadBytes: 0
        }
    }

    // Declared inline so the dialog gets a real Popup parent — the dialog caps
    // itself against parent.height, and createObject() on a Popup leaves that
    // unset. The host's height is this suite's stand-in for the window.
    Component {
        id: hostComp
        Item {
            property alias dialog: dlg
            width: 900
            AddApplicationDialog { id: dlg }
        }
    }

    function modelWith(n) {
        var m = modelComp.createObject(testCase);
        for (var i = 0; i < n; ++i)
            m.append({ name: "dep_" + i, displayName: "Dependency " + i,
                       description: "a required package", action: "install",
                       toVersion: "1.0.0", isInstalled: false, resolverError: "" });
        return m;
    }

    function openWithDeps(n, windowHeight) {
        var host = hostComp.createObject(testCase, {
            height: windowHeight === undefined ? testCase.roomyWindow : windowHeight
        });
        host.dialog.requiredPackagesModel = modelWith(n);
        host.dialog.openWith({ name: "target_app", displayName: "Target App",
                               description: "the app being added",
                               selectedVersion: "2.0.0", versions: [] });
        waitForRendering(testCase);
        return host;
    }

    function cleanupHost(host) {
        var m = host.dialog.requiredPackagesModel;
        host.destroy();
        if (m)
            m.destroy();
    }

    // A Dialog is a Popup, not an Item: its content lives under contentItem and
    // never appears in the object tree findChild() walks.
    function deepFind(node, name) {
        if (!node)
            return null;
        if (node.objectName === name)
            return node;

        var kids = [];
        if (node.children)
            for (var i = 0; i < node.children.length; ++i)
                kids.push(node.children[i]);
        if (node.contentItem && kids.indexOf(node.contentItem) < 0)
            kids.push(node.contentItem);

        for (var j = 0; j < kids.length; ++j) {
            var hit = deepFind(kids[j], name);
            if (hit)
                return hit;
        }
        return null;
    }

    function find(host, name) {
        var item = null;
        tryVerify(function () {
            item = deepFind(host.dialog.contentItem, name);
            return item !== null && item.height > 0;
        }, 5000, name + " is realised");
        return item;
    }

    function listOf(host) {
        return find(host, "addApplicationDialog.requiredPackages");
    }

    function test_a_long_list_is_bounded_and_scrollable() {
        var host = openWithDeps(20);
        var list = listOf(host);

        compare(list.contentHeight, 20 * testCase.rowHeight, "all 20 rows are in the list");
        compare(list.height, testCase.maxRows * testCase.rowHeight,
                "the list stops at its cap instead of growing");

        // Both halves matter. Bounded but not flickable is a list with rows the
        // user can see the edge of and cannot reach.
        verify(list.interactive, "flickable, because there is something out of view");

        var bar = list.ScrollBar.vertical;
        verify(bar !== null && bar.visible, "and says so with a scrollbar");

        cleanupHost(host);
    }

    // The scrollbar is an overlay: it paints on top of the delegates, at the
    // view's right edge. With the view inset by the dialog's content margin it
    // sat 20px in, squarely over each row's stage badge — and read as floating
    // rather than attached to anything. The view is full-bleed now and the
    // margin lives on the rows, so the bar hugs the dialog edge with the row
    // content stopping short of it.
    function test_the_scrollbar_hugs_the_edge_and_clears_the_row_content() {
        var host = openWithDeps(20);
        var list = listOf(host);

        compare(list.width, host.dialog.width,
                "the view is full-bleed, so the bar is at the dialog's edge");

        var bar = list.ScrollBar.vertical;
        verify(bar !== null && bar.visible, "the bar is showing");

        var row = list.itemAtIndex(0);
        verify(row !== null, "the first row is realised");

        // Delegates sit at x=0 and span the view, so the row's contentItem is
        // already in the view's coordinate space.
        var contentRight = row.contentItem.x + row.contentItem.width;
        verify(contentRight <= bar.x,
               "row content ends at " + contentRight + ", scrollbar starts at "
               + bar.x);

        cleanupHost(host);
    }

    function test_every_dependency_can_be_reached() {
        var host = openWithDeps(20);
        var list = listOf(host);

        list.positionViewAtEnd();
        waitForRendering(testCase);

        compare(list.contentY, list.contentHeight - list.height,
                "scrolls all the way to the last row");
        verify(list.itemAtIndex(19) !== null, "the twentieth dependency is realised");

        cleanupHost(host);
    }

    // The actual reported symptom: the buttons and the footer left the screen.
    // Asserted as "the dialog is the same height either way" rather than against
    // a pixel value, because what broke was the dependence itself.
    function test_the_dialog_does_not_grow_with_the_dependency_count() {
        var few = openWithDeps(8);
        listOf(few);
        var fewHeight = few.dialog.height;

        var many = openWithDeps(40);
        listOf(many);

        compare(many.dialog.height, fewHeight,
                "40 dependencies make the dialog no taller than 8");
        verify(fewHeight > 0, "and it is a real measurement");

        cleanupHost(few);
        cleanupHost(many);
    }

    // The cap must not become a permanent scrollbar on lists that fit. A short
    // list that steals wheel events from the dialog it sits in is a regression
    // in the other direction.
    function test_a_short_list_neither_scrolls_nor_reserves_room() {
        var host = openWithDeps(3);
        var list = listOf(host);

        compare(list.height, 3 * testCase.rowHeight, "sized to its contents");
        compare(list.height, list.contentHeight, "nothing out of view");
        verify(!list.interactive, "so not flickable");

        var bar = list.ScrollBar.vertical;
        verify(bar === null || !bar.visible, "and no scrollbar");

        cleanupHost(host);
    }

    // Exactly at the cap is the boundary the two branches meet at, and the one
    // an off-by-one in the bound would put on the wrong side.
    function test_a_list_exactly_at_the_cap_still_fits() {
        var host = openWithDeps(testCase.maxRows);
        var list = listOf(host);

        compare(list.height, list.contentHeight, "the last row that fits, fits");
        verify(!list.interactive, "and needs no scrolling");

        cleanupHost(host);
    }

    function test_the_dialog_never_outgrows_its_window() {
        var host = openWithDeps(20, testCase.crampedWindow);

        verify(host.dialog.height <= host.height,
               "dialog " + host.dialog.height + "px in a "
               + host.height + "px window");
        verify(host.dialog.height < host.dialog.implicitHeight,
               "and it is capped, not merely small");

        cleanupHost(host);
    }

    // The load-bearing one. A ColumnLayout takes a shortfall out of every child
    // in proportion to the slack each has, and Layout.minimumHeight defaults to
    // 0 — so without an explicit minimum on every other section, a short window
    // squashes the showcase and clips the footer while the list sails on at six
    // rows. This asserts the shortfall lands where it was aimed.
    function test_a_short_window_takes_it_out_of_the_list_only() {
        var host = openWithDeps(20, testCase.crampedWindow);
        var list = listOf(host);

        verify(list.height < testCase.maxRows * testCase.rowHeight,
               "the list gave up room, got " + list.height + "px");
        verify(list.height >= testCase.rowHeight,
               "but never below one row, got " + list.height + "px");
        verify(list.interactive, "and is still reachable by scrolling");

        var showcase = find(host, "addApplicationDialog.showcase");
        compare(showcase.height, testCase.showcaseHeight, "the showcase did not squash");

        var footer = find(host, "addApplicationDialog.footerText");
        compare(footer.height, footer.implicitHeight,
                "the footer summary is not clipped");

        cleanupHost(host);
    }

    function test_optional_rows_match_required_layout_and_scroll_in_a_short_window() {
        var host = openWithDeps(20, testCase.crampedWindow);
        var optionals = [];
        for (var i = 0; i < 8; ++i)
            optionals.push({name: "optional_" + i, displayName: "Optional " + i,
                version: "1.0.0", action: "install", description: "an optional package",
                versions: [{manifest: {version: "1.0.0"}}]});
        host.dialog.metadata = Object.assign({}, host.dialog.metadata, {optionalPackages: optionals});
        var required = listOf(host);
        var optional = find(host, "addApplicationDialog.optionalPackages");
        compare(optional.width, required.width);
        compare(optional.itemAtIndex(0).height, required.itemAtIndex(0).height);
        compare(optional.itemAtIndex(0).leftPadding, required.itemAtIndex(0).leftPadding);
        compare(optional.itemAtIndex(0).rightPadding, required.itemAtIndex(0).rightPadding);
        verify(optional.height >= testCase.rowHeight);
        verify(required.height >= testCase.rowHeight);
        verify(optional.interactive);
        verify(host.dialog.height <= host.height);
        compare(find(host, "addApplicationDialog.showcase").height, testCase.showcaseHeight);
        var footer = find(host, "addApplicationDialog.footerText");
        compare(footer.height, footer.implicitHeight);
        optional.positionViewAtEnd();
        waitForRendering(testCase);
        verify(optional.itemAtIndex(7) !== null);
        cleanupHost(host);
    }

    // An embedded module the catalog lacks: kept as installed, nothing to pick.
    function test_installed_only_optional_has_no_choices() {
        var host = openWithDeps(1, testCase.crampedWindow);
        host.dialog.metadata = Object.assign({}, host.dialog.metadata, {optionalPackages: [
            {name: "modules_state", displayName: "Module State", version: "0.1.0",
             installedVersion: "0.1.0", installedOnly: true, action: "installed",
             selected: false, versions: []}]});
        waitForRendering(testCase);
        var box = find(host, "addApplicationDialog.optional.modules_state");
        verify(box.checked, "kept, whatever the backend's default");
        verify(!box.enabled, "nothing to opt out of");
        var combo = find(host, "packageRow.version.modules_state");
        verify(!combo.visible, "no version picker");
        cleanupHost(host);
    }

    SignalSpy { id: openedSpy; signalName: "opened" }
    SignalSpy { id: requestSpy; signalName: "versionChangeRequested" }

    // PackageCoordinator::emitDialogMetadata for an installed target_app 1.2.0
    // with one installed optional and one it does not have yet.
    function installedAppMetadata(releasedAt, installStatus, pending) {
        return { name: "target_app", displayName: "Target App", repositoryUrl: "https://repo/",
                 selectedVersion: "1.2.0", installedVersion: "1.2.0", latestVersion: "1.2.0",
                 installStatus: installStatus,
                 isInstalled: installStatus === InstallStatus.Installed,
                 resolutionPending: pending, optionalChangesPending: false,
                 versions: [{ manifest: { version: "1.2.0" }, releasedAt: releasedAt }],
                 optionalPackages: [
                     { name: "extra_module", displayName: "Extra", version: "1.0.0",
                       installedVersion: "1.0.0", action: "installed", selected: true,
                       versions: [{ manifest: { version: "1.0.0" } }] },
                     { name: "other_module", displayName: "Other", version: "2.0.0",
                       action: "install", selected: false,
                       versions: [{ manifest: { version: "2.0.0" } },
                                  { manifest: { version: "1.9.0" } }] }] };
    }

    // OverlayDialogs.onAddApplicationDataUpdated.
    function deliver(host, metadata) {
        host.dialog.metadata = metadata;
        host.dialog.installStage = metadata.installStage || InstallStage.None;
        waitForRendering(testCase);
    }

    // The catalog moves under an open dialog (PackageCoordinator::refreshActiveAddDialog):
    // the app was republished under the same version, so the backend sends the
    // same app again, pending then resolved, with no requestOpen. The header and
    // button follow it; the user's optional choices stay, and nothing reopens.
    function test_a_catalog_change_while_open_turns_launch_into_reinstall() {
        var host = hostComp.createObject(testCase, { height: testCase.roomyWindow });
        host.dialog.requiredPackagesModel = modelWith(1);
        openedSpy.target = host.dialog;
        openedSpy.clear();
        requestSpy.target = host.dialog;
        requestSpy.clear();

        var before = "2026-09-01T12:00:00Z";
        var after = "2026-10-05T12:00:00Z";
        host.dialog.openWith(installedAppMetadata(before, InstallStatus.Installed, true));
        deliver(host, installedAppMetadata(before, InstallStatus.Installed, false));
        tryCompare(openedSpy, "count", 1);
        var primary = find(host, "addApplicationDialog.primaryButton");
        compare(primary.text, "Launch");

        // The user opts out of one optional and pins another's version.
        mouseClick(find(host, "addApplicationDialog.optional.extra_module"));
        compare(requestSpy.count, 1, "the opt-out re-resolves");
        compare(requestSpy.signalArguments[0][3], { extra_module: false });
        find(host, "packageRow.version.other_module").activated(1);
        compare(requestSpy.count, 2, "the version pick re-resolves");
        compare(requestSpy.signalArguments[1][3], { extra_module: false });
        compare(requestSpy.signalArguments[1][4], { other_module: "1.9.0" });
        deliver(host, installedAppMetadata(before, InstallStatus.Installed, false));
        compare(primary.text, "Launch", "neither choice changes anything installed");

        deliver(host, installedAppMetadata(after, InstallStatus.DifferentHash, true));
        compare(primary.text, "Checking packages\u2026");
        verify(!primary.enabled, "no install from a plan that is still resolving");
        compare(find(host, "addApplicationDialog.releasedText").text,
                new Date(after).toLocaleDateString(Qt.locale(), Locale.ShortFormat),
                "the header follows the new catalog row at once");

        deliver(host, installedAppMetadata(after, InstallStatus.DifferentHash, false));
        compare(primary.text, "Reinstall");
        verify(primary.enabled);

        verify(host.dialog.visible, "still open");
        compare(openedSpy.count, 1, "and never reopened");
        compare(requestSpy.count, 2, "the refresh asks the backend for nothing");
        verify(!find(host, "addApplicationDialog.optional.extra_module").checked,
               "the opt-out survives");
        compare(find(host, "packageRow.version.other_module").displayText, "v.1.9.0",
                "and so does the pin");

        cleanupHost(host);
    }
}
