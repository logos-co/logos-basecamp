import QtQuick
import QtTest
import Basecamp.AppManager
import Basecamp.Backend

// The App Manager grid's geometry: cells sized from the width actually
// available, and an app name that uses the cell rather than the icon under it.
TestCase {
    id: testCase
    name: "AppGridLayout"
    when: windowShown

    readonly property string veryLongName:
        "Extraordinarily Long Application Display Name That Cannot Possibly Fit"

    function appData(name, displayName) {
        return {
            name: name,
            displayName: displayName,
            iconUrl: "",
            description: "",
            repositoryUrl: "https://example.test/logos-repo.json",
            installType: "user",
            isInstalled: true,
            hasUpdate: false,
            installStatus: InstallStatus.Installed,
            planInstallStage: InstallStage.None,
            planDownloadReceived: 0,
            planDownloadTotal: 0,
            hasInstallableVersion: true,
            supportsFullBleedIcon: false,
        };
    }

    function findByObjectName(item, target) {
        if (!item)
            return null;
        if (item.objectName === target)
            return item;
        var kids = item.children || [];
        for (var i = 0; i < kids.length; ++i) {
            var hit = findByObjectName(kids[i], target);
            if (hit)
                return hit;
        }
        if (item.contentItem)
            return findByObjectName(item.contentItem, target);
        return null;
    }

    function gridLabel(delegate) {
        var label = findByObjectName(delegate, "appGridDelegate.label");
        verify(label, "found the grid delegate's name label");
        return label;
    }

    Component {
        id: gridDelegateComp
        AppGridDelegate {}
    }

    Component {
        id: listDelegateComp
        AppListDelegate {}
    }

    Component {
        id: gridComp
        AppGrid {}
    }

    ListModel { id: emptyModel }

    // ── The cell width follows the available width ───────────────────────────
    function test_columns_follow_the_available_width() {
        var model = Qt.createQmlObject(
            'import QtQuick; ListModel {}', testCase, "gridModel");
        for (var i = 0; i < 6; ++i)
            model.append({ name: "app_" + i, displayName: "App " + i });

        var grid = createTemporaryObject(gridComp, testCase, {
            width: 1000, height: 600, modulesSource: model,
        });
        verify(grid, "grid created");
        var cellHeight = 162;

        compare(grid.implicitHeight, 2 * cellHeight,
                "five columns at 1000px");
        grid.width = 400;
        tryCompare(grid, "implicitHeight", 3 * cellHeight, 2000,
                   "two columns at 400px");

        grid.width = 120;
        tryCompare(grid, "implicitHeight", 6 * cellHeight, 2000,
                   "one column below the minimum cell width");
    }

    function test_cells_absorb_the_leftover_width() {
        var model = Qt.createQmlObject(
            'import QtQuick; ListModel {}', testCase, "widthModel");
        model.append({ name: "app_0", displayName: "App" });

        var grid = createTemporaryObject(gridComp, testCase, {
            width: 1000, height: 400, modulesSource: model,
        });
        var delegate = findByObjectName(grid, "appManager.app.app_0");
        verify(delegate, "delegate instantiated");
        compare(delegate.width, 200, "1000px over 5 columns");

        grid.width = 900;
        tryCompare(delegate, "width", 180, 2000, "900px divides exactly");
    }

    // ── The name uses the cell, not the icon ────────────────────────────────
    function test_label_is_wider_than_the_icon_it_sits_under() {
        var delegate = createTemporaryObject(gridDelegateComp, testCase, {
            width: 200, height: 162, appData: appData("app", "App"),
        });
        var label = gridLabel(delegate);
        verify(label.width > delegate.tileSize,
               "label " + label.width + "px is wider than the "
               + delegate.tileSize + "px tile");
        // Spans the cell less the delegate's side margins (spacing.small each).
        compare(label.width, 200 - 8 * 2, "label fills the cell minus margins");
    }

    function test_short_name_is_one_line_and_not_truncated() {
        var delegate = createTemporaryObject(gridDelegateComp, testCase, {
            width: 200, height: 162, appData: appData("chat", "Chat"),
        });
        var label = gridLabel(delegate);
        compare(label.lineCount, 1, "short name needs one line");
        compare(label.truncated, false, "and is not elided");
    }

    // ── The very long name ──────────────────────────────────────────────────
    function test_very_long_name_wraps_to_two_lines_then_elides() {
        var delegate = createTemporaryObject(gridDelegateComp, testCase, {
            width: 200, height: 162,
            appData: appData("long_app", testCase.veryLongName),
        });
        var label = gridLabel(delegate);

        compare(label.lineCount, 2,
                "wraps to the second line rather than eliding on the first");
        compare(label.truncated, true,
                "and elides once the second line is full");
        verify(label.width <= 200,
               "never wider than its cell (" + label.width + "px)");
        compare(label.maximumLineCount, 2, "capped at two lines");
    }

    function test_wrapped_name_stays_inside_the_cell_height() {
        var delegate = createTemporaryObject(gridDelegateComp, testCase, {
            width: 200, height: 162,
            appData: appData("long_app", testCase.veryLongName),
        });
        var label = gridLabel(delegate);
        var bottom = label.mapToItem(delegate, 0, label.height).y;
        verify(bottom <= delegate.height,
               "name ends at " + bottom + "px, inside the "
               + delegate.height + "px cell");
    }

    function test_one_long_token_still_uses_both_lines() {
        var delegate = createTemporaryObject(gridDelegateComp, testCase, {
            width: 200, height: 162,
            appData: appData("t", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        });
        var label = gridLabel(delegate);
        compare(label.wrapMode, Text.Wrap, "wraps anywhere when it must");
        compare(label.lineCount, 2, "uses the second line");
    }

    // ── List view is deliberately unchanged ─────────────────────────────────
    function test_list_row_name_stays_on_one_line() {
        var delegate = createTemporaryObject(listDelegateComp, testCase, {
            width: 800, height: 64,
            appData: appData("long_app", testCase.veryLongName),
        });
        verify(delegate, "list delegate created");
        var label = findByObjectName(delegate, "appListDelegate.label");
        if (!label) {
            compare(delegate.height, 64, "list row height unchanged");
            return;
        }
        compare(label.lineCount, 1, "list names do not wrap");
        compare(label.truncated, true, "they elide");
    }
}
