import QtQuick
import QtTest

// Directory import, not `import Basecamp.Shell`: the shell's QML is compiled
// into main_ui as a qt_add_qml_module, so there is no importable module here —
// only the source directory.
import "../../src/Basecamp/Shell"

// The provider chooser, instantiated for real.
//
// This is the dialog that stands between one app asking for a capability and
// another app being handed it, so the assertions here are about what the user
// is shown and what a click reports — not about broker policy, which is
// unit-tested against fakes.
TestCase {
    id: testCase
    name: "IntentChooserDialog"
    when: windowShown
    width: 640
    height: 480

    readonly property var twoProviders: [
        { moduleName: "wallet_a", displayName: "Wallet A", iconSource: "" },
        { moduleName: "wallet_b", displayName: "Wallet B", iconSource: "" }
    ]

    Component {
        id: dialogComp
        IntentChooserDialog {
            displayNameLookup: function (name) { return name; }
        }
    }

    // A Dialog is a Popup, not an Item: its content lives under contentItem and
    // never appears in the object-tree findChild() walks. ListView delegates sit
    // another level down again, under the view's own contentItem.
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

    function test_details_button_is_scaled_to_its_row() {
        var dlg = dialogComp.createObject(testCase);
        dlg.openWith({ dispatchId: "d-1", intent: "wallet.send", requesterName: "chat_ui", providers: testCase.twoProviders });

        waitForRendering(testCase);

        var row = null, btn = null;
        tryVerify(function () {
            row = deepFind(dlg.contentItem, "intentProvider_wallet_a");
            btn = deepFind(dlg.contentItem, "intentProviderDetails_wallet_a");
            return row !== null && btn !== null && btn.height > 0 && row.height > 0;
        }, 5000, "the row and its Details button are realised");

        // LogosButton's implicit floors are 100x44, sized for a dialog's footer
        // actions. In a 56px row that leaves nothing once ItemDelegate's
        // padding is taken off, and the button reads as filling the row rather
        // than sitting in it. It must stay meaningfully shorter than the row.
        verify(btn.height < row.height - 16,
               "Details (" + btn.height + "px) should be well inside the row ("
               + row.height + "px)");

        // …and be centred in it. Compare midpoints in the row's own coordinate
        // space, which is what the eye actually judges.
        var mid = btn.mapToItem(row, 0, btn.height / 2).y;
        fuzzyCompare(mid, row.height / 2, 2,
                     "Details is not vertically centred in its row");

        dlg.destroy();
    }

    function test_details_expands_in_place_without_ending_the_request() {
        var dlg = dialogComp.createObject(testCase);
        dlg.detailsLookup = function (name) {
            return { moduleName: name, version: "1.2.3",
                     repositoryUrl: "https://packages.example.org/logos-repo.json",
                     verified: false };
        };
        dlg.openWith({ dispatchId: "d-2", intent: "wallet.send", requesterName: "chat_ui", providers: testCase.twoProviders });

        waitForRendering(testCase);

        var btn = null;
        tryVerify(function () {
            btn = deepFind(dlg.contentItem, "intentProviderDetails_wallet_b");
            return btn !== null && btn.width > 0;
        }, 5000, "second row's Details button exists");

        mouseClick(btn);

        // Expands the row it belongs to, and only that one.
        compare(dlg.expandedProvider, "wallet_b");

        // And the dialog is STILL OPEN. Inspecting a provider used to cancel
        // the request to navigate away, so the user lost the question they went
        // to answer.
        verify(dlg.visible, "reading about a provider does not end the request");

        // Toggling closes it again rather than stacking panels.
        mouseClick(btn);
        compare(dlg.expandedProvider, "");

        dlg.destroy();
    }

    function test_an_expanded_row_can_still_be_chosen() {
        var dlg = dialogComp.createObject(testCase);
        var spy = chosenSpy.createObject(testCase, { target: dlg });
        dlg.detailsLookup = function (name) { return { moduleName: name }; };
        dlg.openWith({ dispatchId: "d-9", intent: "wallet.send", requesterName: "chat_ui", providers: testCase.twoProviders });
        waitForRendering(testCase);

        var row = null;
        tryVerify(function () {
            row = deepFind(dlg.contentItem, "intentProvider_wallet_a");
            return row !== null && row.height > 0;
        }, 5000, "row realised");

        dlg.toggleDetails("wallet_a");
        waitForRendering(testCase);

        // You open the details to DECIDE. Requiring the user to collapse them
        // again before they may act on that decision is a dead end — an earlier
        // version blocked the click outright to stop stray selections, which
        // stopped the deliberate ones too.
        mouseClick(row, 10, 10);

        compare(spy.count, 1, "an expanded row still chooses");
        compare(spy.signalArguments[0][1], "wallet_a");

        spy.destroy();
        dlg.destroy();
    }

    function test_dismissing_cancels_rather_than_going_silent() {
        var dlg = dialogComp.createObject(testCase);
        var spy = cancelSpy.createObject(testCase, { target: dlg });

        dlg.openWith({ dispatchId: "d-3", intent: "wallet.send", requesterName: "chat_ui", providers: testCase.twoProviders });
        // Escape, or the dialog being torn down. A request is parked behind
        // this, so silence would leave it waiting for the broker's backstop.
        dlg.close();

        compare(spy.count, 1, "choiceCancelled fired on a non-button close");
        compare(spy.signalArguments[0][0], "d-3");

        spy.destroy();
        dlg.destroy();
    }

    function test_closeFor_ignores_a_stale_dispatch_id() {
        var dlg = dialogComp.createObject(testCase);
        dlg.openWith({ dispatchId: "d-4", intent: "wallet.send", requesterName: "chat_ui", providers: testCase.twoProviders });

        dlg.closeFor("some-older-dispatch");
        verify(dlg.visible, "still open for an unrelated id");

        dlg.closeFor("d-4");
        verify(!dlg.visible, "closed for its own id");

        dlg.destroy();
    }

    // Both fixture providers carry iconSource: "" — the ordinary case, and the
    // one that used to render nothing. A bare Image with an empty source is
    // invisible, so the slot was collapsed to zero width to avoid a blank gap,
    // and an app with no artwork appeared in this dialog as text alone while the
    // sidebar and App Manager drew it a monogram. The dialog's own header claims
    // a provider cannot be presented differently here than elsewhere; that was
    // true of the name and not the icon.
    function test_a_provider_without_an_icon_still_gets_a_tile() {
        var dlg = dialogComp.createObject(testCase);
        dlg.openWith({ dispatchId: "d-5", intent: "wallet.send", requesterName: "chat_ui", providers: testCase.twoProviders });

        waitForRendering(testCase);

        var tile = null;
        tryVerify(function () {
            tile = deepFind(dlg.contentItem, "intentProviderIcon_wallet_a");
            return tile !== null && tile.height > 0;
        }, 5000, "the provider's icon tile is realised");

        verify(tile.showsMonogram, "falls back to a monogram rather than nothing");
        verify(tile.monogram.length > 0, "the monogram is not blank");
        verify(tile.width > 0, "the slot is not collapsed");
        compare(tile.width, tile.height, "square");

        // Derived from the row it sits in rather than hardcoded, so the two
        // cannot drift apart. Bounds, not an exact figure: the row's vertical
        // padding is ItemDelegate's default and may move with the Qt version.
        verify(tile.width >= 20 && tile.width <= 32,
               "tile sized within its row, got " + tile.width);
        verify(tile.width <= dlg.contentItem.height,
               "tile never outgrows the dialog body");

        dlg.destroy();
    }

    Component { id: cancelSpy;  SignalSpy { signalName: "choiceCancelled" } }
    Component { id: chosenSpy;  SignalSpy { signalName: "providerChosen" } }
}
