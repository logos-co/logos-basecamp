import QtQuick
import QtTest
import Basecamp.Backend
import Logos.Theme

// Directory import, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"

// The welcome page's app tile: a name that uses the cell rather than the icon
// under it, and a badge that is part of the tile.
TestCase {
    id: testCase
    // A desktop window: below 600 px dialogs and menus are phone sheets.
    width: 900
    height: 700
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

    // An app without an icon has no colour of its own, installed or not, as
    // the sidebar shows it.
    function test_a_tile_without_an_icon_has_no_colour_of_its_own() {
        for (const installed of [false, true]) {
            const data = appData("wallet_ui", "Wallet");
            data.isInstalled = installed;
            data.installStatus = installed ? InstallStatus.Installed : InstallStatus.NotInstalled;
            const delegate = createTemporaryObject(gridDelegateComp, testCase, { appData: data });
            const tile = findByObjectName(delegate, "appGridDelegate.tile");
            verify(tile, "found the tile");
            verify(Qt.colorEqual(tile.fallbackColor, Theme.palette.surfaceRaised),
                   "installed " + installed + ": " + tile.fallbackColor);
        }
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

    // ── A click on the state badge is a click on the app ─────────────────────
    // The badge ("Upgrade", "Install", …) sits on top of the tile. A
    // HoverHandler on it made it take the press: clicking the word did
    // nothing, only the icon around it opened the app.
    SignalSpy { id: appClickedSpy; signalName: "appClicked" }

    function updatable() {
        var data = appData("wallet", "Wallet");
        data.hasUpdate = true;
        data.installStatus = InstallStatus.UpgradeAvailable;
        return data;
    }

    function clickBadge(delegate, badgeName) {
        var badge = findByObjectName(delegate, badgeName);
        verify(badge && badge.visible, badgeName + " is shown");
        compare(badge.text, "Upgrade");
        appClickedSpy.target = delegate;
        appClickedSpy.clear();
        mouseClick(badge);
        compare(appClickedSpy.count, 1, "clicking the badge opens the app");
        compare(appClickedSpy.signalArguments[0][0], "wallet");
    }

    function test_clicking_the_tile_badge_opens_the_app() {
        var tile = createTemporaryObject(gridDelegateComp, testCase.Window.window.contentItem, {
            width: 200, height: 162, appData: updatable(),
        });
        waitForRendering(tile);
        clickBadge(tile, "appGridStateBadge");
    }
}
