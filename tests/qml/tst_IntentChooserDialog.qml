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

    readonly property var oneProvider: [
        { moduleName: "wallet_a", displayName: "Wallet A", iconSource: "" }
    ]

    // The broker sends a lone provider here on purpose, so that the quiet case
    // is not the unguarded one. But then it is asking for consent, not a pick,
    // and "Choose an app" over a list of one names an action the user cannot
    // take.
    function test_the_title_asks_to_confirm_when_there_is_nothing_to_choose() {
        var dlg = dialogComp.createObject(testCase);

        dlg.openWith({ dispatchId: "d-t1", intent: "wallet.send",
                       requesterName: "chat_ui", providers: testCase.oneProvider });
        compare(dlg.title, "Use this app?");

        // …and back, so the title tracks the offer rather than being set once.
        dlg.openWith({ dispatchId: "d-t2", intent: "wallet.send",
                       requesterName: "chat_ui", providers: testCase.twoProviders });
        compare(dlg.title, "Choose an app");

        dlg.destroy();
    }

    // Approving used to be a click on a list row while refusing was a button —
    // the safe action prominent, the intended one an affordance that does not
    // look like one. With a single provider there is no ambiguity about what an
    // affirmative button would mean, so there is one.
    function test_a_lone_provider_can_be_approved_with_a_button() {
        var dlg = dialogComp.createObject(testCase);
        var spy = chosenSpy.createObject(testCase, { target: dlg });

        dlg.openWith({ dispatchId: "d-ok", intent: "wallet.send",
                       requesterName: "chat_ui", providers: testCase.oneProvider });
        waitForRendering(testCase);

        var confirm = null;
        tryVerify(function () {
            confirm = deepFind(dlg.footerItem, "intentChooserConfirm");
            return confirm !== null && confirm.width > 0;
        }, 5000, "the confirm button is realised");

        verify(confirm.visible, "shown when there is exactly one provider");
        mouseClick(confirm);

        // The same answer a row click gives, carrying the broker's dispatch id.
        compare(spy.count, 1, "answers once");
        compare(spy.signalArguments[0][0], "d-ok");
        compare(spy.signalArguments[0][1], "wallet_a");
        verify(!dlg.visible, "and closes, like any other answer");

        spy.destroy();
        dlg.destroy();
    }

    // With several, a button could not say which one it meant: the chooser has
    // no selection state — a row click IS the answer. Adding one would mean
    // changing how the consent is expressed, not just adding a control.
    function test_several_providers_get_no_affirmative_button() {
        var dlg = dialogComp.createObject(testCase);
        var spy = chosenSpy.createObject(testCase, { target: dlg });

        dlg.openWith({ dispatchId: "d-many", intent: "wallet.send",
                       requesterName: "chat_ui", providers: testCase.twoProviders });
        waitForRendering(testCase);

        var confirm = deepFind(dlg.footerItem, "intentChooserConfirm");
        verify(confirm !== null, "the button exists in the tree");
        verify(!confirm.visible, "but is not offered with more than one provider");

        // Cancel is still the only button, and still answers.
        var cancel = deepFind(dlg.footerItem, "intentChooserCancel");
        verify(cancel !== null && cancel.visible, "Cancel is unchanged");
        compare(spy.count, 0, "nothing answered by merely opening");

        spy.destroy();
        dlg.destroy();
    }

    readonly property var oneInstallable: [
        { moduleName: "wallet_c", displayName: "Secure Wallet",
          repositoryLabel: "Logos Official Modules",
          repositoryLink: "https://github.com/logos-co/logos-modules-release" }
    ]

    // The mixed case. One installed provider used to render exactly like "this
    // is the only app that can do this", and nothing else in the UI says what a
    // package provides — so the alternatives were unreachable, not just unshown.
    function test_catalog_packages_appear_in_their_own_section() {
        var dlg = dialogComp.createObject(testCase, {
            installableLookup: function (intent) { return testCase.oneInstallable; }
        });
        dlg.openWith({ dispatchId: "d-mix", intent: "wallet.send",
                       requesterName: "chat_ui", providers: testCase.oneProvider });
        waitForRendering(testCase);

        var header = null, row = null;
        tryVerify(function () {
            header = deepFind(dlg.contentItem, "intentChooserInstallableHeader");
            row = deepFind(dlg.contentItem, "intentInstallable_wallet_c");
            return header !== null && row !== null && row.height > 0;
        }, 5000, "the not-installed section is realised");

        verify(header.visible, "the section is labelled, not merged into the list");

        // The installed provider is still the one that can service this.
        verify(deepFind(dlg.contentItem, "intentProvider_wallet_a") !== null,
               "the real provider is still listed");

        dlg.destroy();
    }

    function test_no_section_when_the_catalog_adds_nothing() {
        var dlg = dialogComp.createObject(testCase);
        dlg.openWith({ dispatchId: "d-none", intent: "wallet.send",
                       requesterName: "chat_ui", providers: testCase.oneProvider });
        waitForRendering(testCase);

        var header = deepFind(dlg.contentItem, "intentChooserInstallableHeader");
        verify(header !== null && !header.visible,
               "no heading over an empty section");

        dlg.destroy();
    }

    // The load-bearing one. An uninstalled package cannot be dispatched to, so
    // picking it has to END the request — and it must do so BEFORE asking for
    // the install, or the broker is left holding an AwaitingChoice that has no
    // deadline (a human is deciding) while a download runs.
    function test_installing_instead_answers_the_request_first() {
        var dlg = dialogComp.createObject(testCase, {
            installableLookup: function (intent) { return testCase.oneInstallable; }
        });
        var cancelled = cancelSpy.createObject(testCase, { target: dlg });
        var install = installSpy.createObject(testCase, { target: dlg });
        var chosen = chosenSpy.createObject(testCase, { target: dlg });

        dlg.openWith({ dispatchId: "d-inst", intent: "wallet.send",
                       requesterName: "chat_ui", providers: testCase.oneProvider });
        waitForRendering(testCase);

        var action = null;
        tryVerify(function () {
            action = deepFind(dlg.contentItem, "intentInstallableAction_wallet_c");
            return action !== null && action.width > 0;
        }, 5000, "the Install… affordance is realised");

        mouseClick(action);

        compare(cancelled.count, 1, "the request is answered");
        compare(cancelled.signalArguments[0][0], "d-inst");
        compare(install.count, 1, "and the install is asked for");
        compare(install.signalArguments[0][0], "wallet.send");
        compare(install.signalArguments[0][1], "wallet_c");

        // Never dispatched: nothing installed could have serviced this pick.
        compare(chosen.count, 0, "no provider was chosen");

        // And closing must not send a SECOND, contradictory cancel.
        verify(!dlg.visible, "the dialog closed");
        compare(cancelled.count, 1, "answered exactly once");

        chosen.destroy(); install.destroy(); cancelled.destroy();
        dlg.destroy();
    }

    // Whatever closed it, nothing of the finished request stays addressable —
    // and the catalog rows do not sit there stale waiting for the next offer.
    function test_closing_leaves_nothing_of_the_request_behind() {
        var dlg = dialogComp.createObject(testCase, {
            installableLookup: function (intent) { return testCase.oneInstallable; }
        });

        dlg.openWith({ dispatchId: "d-left", intent: "wallet.send",
                       requesterName: "chat_ui", providers: testCase.oneProvider });
        compare(dlg.dispatchId, "d-left");
        compare(dlg.installable.length, 1);

        dlg.closeFor("d-left");
        tryVerify(function () { return !dlg.visible; }, 5000, "closed");

        compare(dlg.dispatchId, "", "no id left to answer for");
        compare(dlg.installable.length, 0, "no stale catalog rows");

        dlg.destroy();
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

    // The Details panel's "From" row. It used to print the catalog URL into an
    // elided LogosText, so on GitHub it read as a truncated
    // "https://raw.githubusercontent.com/logos-co/logos-mod…" — unreadable,
    // uncopyable, and naming a CDN rather than a publisher.
    //
    // `repositoryLink` arrives resolved, alongside the rest of the facts; how
    // the shell resolves it is repository_source_test's business. What is
    // pinned here is that the row renders that field rather than the catalog
    // address sitting next to it.
    function test_details_shows_the_resolved_link_and_lets_it_be_copied() {
        var dlg = dialogComp.createObject(testCase);
        dlg.detailsLookup = function (name) {
            return { moduleName: name, version: "1.2.3", verified: false,
                     repositoryUrl: "https://raw.githubusercontent.com/logos-co/"
                                  + "logos-modules-release/refs/heads/main/logos-repo.json",
                     repositoryLabel: "Logos Official Modules",
                     repositoryLink: "https://github.com/logos-co/logos-modules-release" };
        };
        dlg.openWith({ dispatchId: "d-3", intent: "wallet.send", requesterName: "chat_ui",
                       providers: testCase.twoProviders });
        dlg.toggleDetails("wallet_a");
        waitForRendering(testCase);

        var link = null, copy = null;
        tryVerify(function () {
            link = deepFind(dlg.contentItem, "intentProviderSourceLink_wallet_a");
            copy = deepFind(dlg.contentItem, "intentProviderSourceCopy_wallet_a");
            return link !== null && copy !== null && link.text.length > 0;
        }, 5000, "the source row is realised");

        compare(link.text, "https://github.com/logos-co/logos-modules-release");
        verify(link.text.indexOf("raw.githubusercontent.com") < 0,
               "the catalog address is not what is shown");
        verify(link.selectByMouse, "drag-selectable, not just readable");

        var copied = copySpy.createObject(testCase, { target: copy });
        copy.copy();
        compare(copied.count, 1, "the button copies");
        compare(copied.signalArguments[0][0], link.text,
                "what is copied is what was shown");

        copied.destroy();
        dlg.destroy();
    }

    // A provider installed from nowhere the shell knows — sideloaded, or
    // embedded in the app. An empty "From" would be a claim about provenance
    // that the shell cannot make, on the one panel whose job is provenance.
    function test_a_provider_with_no_repository_shows_no_source_row() {
        var dlg = dialogComp.createObject(testCase);
        dlg.detailsLookup = function (name) {
            return { moduleName: name, version: "1.0.0", installType: "embedded",
                     verified: false };
        };
        dlg.openWith({ dispatchId: "d-4", intent: "wallet.send", requesterName: "chat_ui",
                       providers: testCase.twoProviders });
        dlg.toggleDetails("wallet_a");
        waitForRendering(testCase);

        var source = null;
        tryVerify(function () {
            source = deepFind(dlg.contentItem, "intentProviderSource_wallet_a");
            return source !== null;
        }, 5000, "the source row exists in the tree");

        verify(!source.visible, "and is not shown without a repository");

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
    Component { id: copySpy;    SignalSpy { signalName: "copied" } }
    Component { id: installSpy; SignalSpy { signalName: "installRequested" } }
}
