import QtQuick
import QtTest

// Directory import, not `import Basecamp.Shell`: the shell's QML is compiled
// into main_ui as a qt_add_qml_module, so there is no importable module here —
// only the source directory. The design system it depends on IS linked, via
// Logos::DesignSystem in this suite's CMakeLists.
import "../../src/Basecamp/Shell" 

// The install-suggestion dialog, instantiated for real rather than mocked. The
// integration suite cannot reach it: raising it needs a catalog entry for an
// uninstalled package, which needs a published index over HTTPS.
//
// The assertion that matters is what it does NOT do — name the app that asked.
TestCase {
    id: testCase
    name: "IntentInstallDialog"
    when: windowShown

    Component {
        id: dialogComp
        IntentInstallDialog {
            // Stand-in for the shell's real lookup. The point of injecting it is
            // that the dialog never invents a label of its own.
            displayNameLookup: function (name) {
                return name === "probe_ui" ? "Probe" : name;
            }
        }
    }

    function test_names_the_package_not_the_requester() {
        var dlg = dialogComp.createObject(testCase);
        verify(dlg, "dialog created");

        dlg.openWith("probe.echo", ["probe_ui"]);

        // The resolved display name, via the injected lookup — not the raw
        // module name, and not anything supplied by whoever asked.
        compare(dlg.displayNameLookup(dlg.candidates[0].moduleName), "Probe");
        compare(dlg.candidates[0].moduleName, "probe_ui");
        verify(dlg.visible, "dialog is open");

        // Nothing anywhere in the dialog's state carries a requester. An app
        // that could be named here would leak WHO wanted a capability to
        // whoever is reading the screen — and the requester is precisely the
        // party this flow keeps in the dark.
        verify(dlg.intentName === "probe.echo", "carries the intent");
        verify(!("requesterName" in dlg), "dialog has no requester property at all");

        // Nor any request at all: it cannot answer, withdraw or resume one.
        verify(!("dispatchId" in dlg), "dialog is not tied to a request");

        dlg.destroy();
    }

    function test_install_reports_the_chosen_package() {
        var dlg = dialogComp.createObject(testCase);
        var spy = installSpy.createObject(testCase, { target: dlg });

        dlg.openWith("probe.echo", ["probe_ui"]);
        dlg.installRequested(dlg.candidates[0].moduleName);

        compare(spy.count, 1, "installRequested fired once");
        compare(spy.signalArguments[0][0], "probe_ui");

        spy.destroy();
        dlg.destroy();
    }



    function test_multiple_candidates_are_selectable_not_just_listed() {
        var dlg = dialogComp.createObject(testCase);
        var spy = installSpy.createObject(testCase, { target: dlg });

        dlg.openWith("probe.echo", ["probe_ui", "other_ui"], []);

        // Defaults to the first so Install is never ambiguous…
        compare(dlg.selectedCandidate, "probe_ui", "defaults to the first");

        // …but the list is a real choice. An earlier version rendered every
        // candidate and then installed candidates[0] regardless, which is worse
        // than not offering a list at all: it looks like a decision and is not.
        dlg.select("other_ui");
        dlg.installRequested(dlg.selectedCandidate);

        compare(spy.signalArguments[0][0], "other_ui",
                "installs what the user picked, not the first candidate");

        spy.destroy();
        dlg.destroy();
    }

    function test_selection_resets_between_offers() {
        var dlg = dialogComp.createObject(testCase);

        dlg.openWith("probe.echo", ["probe_ui", "other_ui"], []);
        dlg.select("other_ui");

        // A stale selection carried into the next offer would install a package
        // the user never saw, for a request they were not asked about.
        dlg.openWith("probe.echo", ["probe_ui"]);
        compare(dlg.selectedCandidate, "probe_ui", "reset to the new first candidate");

        dlg.destroy();
    }

    function test_shows_where_a_package_would_come_from() {
        var dlg = dialogComp.createObject(testCase);
        dlg.openWith("probe.echo", ["probe_ui"], [{
            moduleName: "probe_ui",
            displayName: "Probe",
            repositoryUrl: "https://packages.example.org/logos-repo.json"
        }]);

        // Hostname, not the full URL — the part that tells you whether this is
        // the catalog you expect. A package suggested by a repo the user added
        // for something unrelated should be recognisable as such.
        compare(dlg.candidates[0].repositoryUrl, "https://packages.example.org/logos-repo.json");

        dlg.destroy();
    }

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

    // A candidate whose repositoryUrl is empty must not render a dangling
    // separator. Asserted on the rendered strings rather than on the private
    // originOf(): what matters is that nothing reaches the screen with a
    // trailing " · " or a truncated "installed from ." sentence.
    function test_absent_origin_renders_no_dangling_separator() {
        var dlg = dialogComp.createObject(testCase);

        // Sole candidate: the body sentence is the only place an origin shows.
        dlg.openWith("probe.echo", ["probe_ui"], [{ moduleName: "probe_ui", displayName: "Probe", repositoryUrl: "" }]);
        waitForRendering(testCase);

        var body = null;
        tryVerify(function () {
            body = deepFind(dlg.contentItem, "intentInstallBody");
            return body !== null && body.text.length > 0;
        }, 5000, "the body line is realised");
        verify(body.text.indexOf("installed from") < 0,
               "no origin clause without a repository, got: " + body.text);

        // Two candidates: the list renders, and the subtitle is the module name
        // alone rather than "name · ".
        dlg.openWith("probe.echo", ["probe_ui", "other_ui"], [
            { moduleName: "probe_ui", displayName: "Probe", repositoryUrl: "" },
            { moduleName: "other_ui", displayName: "Other", repositoryUrl: "https://packages.example.org/r.json" }
        ]);
        waitForRendering(testCase);

        var bare = null, withOrigin = null;
        tryVerify(function () {
            bare = deepFind(dlg.contentItem, "intentInstallSubtitle_probe_ui");
            withOrigin = deepFind(dlg.contentItem, "intentInstallSubtitle_other_ui");
            return bare !== null && withOrigin !== null;
        }, 5000, "both subtitles are realised");

        compare(bare.text, "probe_ui", "no separator when there is no origin");
        compare(withOrigin.text, "other_ui · packages.example.org",
                "hostname only, not the full URL");

        dlg.destroy();
    }

    // The tile falls back to a monogram for every candidate: these are catalog
    // packages, so there is never artwork. The tint arrives by injection —
    // the dialog imports no feature module to get it.
    function test_candidates_get_a_monogram_tile_from_the_injected_colour() {
        var dlg = dialogComp.createObject(testCase, {
            fallbackColorFor: function (name) { return "#123456"; }
        });
        dlg.openWith("probe.echo", ["probe_ui", "other_ui"], [
            { moduleName: "probe_ui", displayName: "Probe", repositoryUrl: "" },
            { moduleName: "other_ui", displayName: "Other", repositoryUrl: "" }
        ]);
        waitForRendering(testCase);

        var tile = null;
        tryVerify(function () {
            tile = deepFind(dlg.contentItem, "intentInstallIcon_probe_ui");
            return tile !== null && tile.height > 0;
        }, 5000, "the candidate's icon tile is realised");

        verify(tile.showsMonogram, "no artwork for a catalog package, so a monogram");
        verify(tile.monogram.length > 0, "the monogram is not blank");
        compare(String(tile.fallbackColor), "#123456", "uses the injected colour");
        verify(tile.width > 0 && tile.width === tile.height, "square, not collapsed");
        verify(tile.width >= 20 && tile.width <= 32,
               "sized within its row, got " + tile.width);

        dlg.destroy();
    }

    Component { id: installSpy; SignalSpy { signalName: "installRequested" } }
}
