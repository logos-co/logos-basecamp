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
            repositoryUrl: "https://packages.example.org/logos-repo.json",
            repositoryLabel: "Example Packages",
            repositoryLink: "https://packages.example.org/logos-repo.json"
        }]);

        // A package suggested by a repo the user added for something unrelated
        // should be recognisable as such before it is installed.
        compare(dlg.candidates[0].repositoryLabel, "Example Packages");
        compare(dlg.candidates[0].repositoryUrl, "https://packages.example.org/logos-repo.json");

        dlg.destroy();
    }

    // The source line used to be a hostname inside the body sentence. For
    // everything published through GitHub that hostname is
    // raw.githubusercontent.com — the same for the official catalog and for
    // anything else on GitHub — so it identified nobody. What stands there now
    // is the publishing repository, resolved by the shell.
    //
    // The dialog is given `repositoryLink` and renders it. That it is not the
    // catalog URL is the shell's doing and is covered by repository_source_test;
    // what is pinned here is that the dialog shows the resolved field and not
    // the address it was handed alongside it.
    function test_source_line_shows_the_resolved_link_not_the_catalog_url() {
        var dlg = dialogComp.createObject(testCase);
        dlg.openWith("probe.echo", ["probe_ui"], [{
            moduleName: "probe_ui",
            displayName: "Probe",
            repositoryUrl: "https://raw.githubusercontent.com/logos-co/"
                         + "logos-modules-release/refs/heads/main/logos-repo.json",
            repositoryLabel: "Logos Official Modules",
            repositoryLink: "https://github.com/logos-co/logos-modules-release"
        }]);
        waitForRendering(testCase);

        var link = null;
        tryVerify(function () {
            link = deepFind(dlg.contentItem, "intentInstallSourceLink");
            return link !== null && link.text.length > 0;
        }, 5000, "the source link is realised");

        compare(link.text, "https://github.com/logos-co/logos-modules-release");
        verify(link.text.indexOf("raw.githubusercontent.com") < 0,
               "the transport host is not what the user is being asked to judge");

        // The body sentence no longer carries an origin clause of its own —
        // two statements of the same fact, one of them unverifiable.
        var body = deepFind(dlg.contentItem, "intentInstallBody");
        verify(body.text.indexOf("installed from") < 0,
               "origin stated once, in the source row, got: " + body.text);

        dlg.destroy();
    }

    // "Go and check where this comes from" happens in another window. A link
    // that has to be retyped by eye is one nobody checks, which makes the line
    // decoration rather than evidence.
    function test_the_source_link_can_be_selected_and_copied() {
        var dlg = dialogComp.createObject(testCase);
        dlg.openWith("probe.echo", ["probe_ui"], [{
            moduleName: "probe_ui",
            displayName: "Probe",
            repositoryUrl: "https://raw.githubusercontent.com/logos-co/"
                         + "logos-modules-release/refs/heads/main/logos-repo.json",
            repositoryLabel: "Logos Official Modules",
            repositoryLink: "https://github.com/logos-co/logos-modules-release"
        }]);
        waitForRendering(testCase);

        var link = null, copy = null;
        tryVerify(function () {
            link = deepFind(dlg.contentItem, "intentInstallSourceLink");
            copy = deepFind(dlg.contentItem, "intentInstallSourceCopy");
            return link !== null && copy !== null && copy.width > 0;
        }, 5000, "the source row is realised");

        verify(link.selectByMouse, "drag-selectable");
        link.selectAll();
        compare(link.selectedText, "https://github.com/logos-co/logos-modules-release",
                "the whole link selects, not a visible fragment of it");

        // The clipboard value is what is on screen. A copy button that yielded
        // the catalog URL instead would hand the user something they did not
        // read and cannot match against the line they clicked next to.
        var copied = copySpy.createObject(testCase, { target: copy });
        copy.copy();
        compare(copied.count, 1, "the button copies");
        compare(copied.signalArguments[0][0], link.text);

        copied.destroy();
        dlg.destroy();
    }

    // With several candidates the source belongs to whichever one Install would
    // act on. A row that kept showing the first candidate's origin after the
    // user picked the second would be describing a package they did not choose.
    function test_the_source_line_follows_the_selection() {
        var dlg = dialogComp.createObject(testCase);
        dlg.openWith("probe.echo", ["probe_ui", "other_ui"], [
            { moduleName: "probe_ui", displayName: "Probe",
              repositoryLabel: "Logos Official",
              repositoryLink: "https://github.com/logos-co/official" },
            { moduleName: "other_ui", displayName: "Other",
              repositoryLabel: "Someone Else's Repo",
              repositoryLink: "https://github.com/someone-else/side-repo" }
        ]);
        waitForRendering(testCase);

        var link = null;
        tryVerify(function () {
            link = deepFind(dlg.contentItem, "intentInstallSourceLink");
            return link !== null && link.text.length > 0;
        }, 5000, "the source link is realised");

        compare(link.text, "https://github.com/logos-co/official");

        dlg.select("other_ui");
        tryVerify(function () {
            return link.text === "https://github.com/someone-else/side-repo";
        }, 5000, "the source tracks the selection, got: " + link.text);

        dlg.destroy();
    }

    // Nothing to show and nothing to copy: an empty row would read as
    // "from nowhere", and a copy button beside it would put "" on the clipboard.
    function test_no_repository_renders_no_source_row() {
        var dlg = dialogComp.createObject(testCase);
        dlg.openWith("probe.echo", ["probe_ui"],
                     [{ moduleName: "probe_ui", displayName: "Probe", repositoryUrl: "" }]);
        waitForRendering(testCase);

        var section = deepFind(dlg.contentItem, "intentInstallSource");
        verify(section !== null, "the source section exists in the tree");
        verify(!section.visible, "but is not shown without a repository");

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
        dlg.openWith("probe.echo", ["probe_ui", "other_ui", "third_ui"], [
            { moduleName: "probe_ui", displayName: "Probe", repositoryLabel: "" },
            { moduleName: "other_ui", displayName: "Other", repositoryLabel: "Example Packages" },
            { moduleName: "third_ui", displayName: "Third", repositoryLabel: "Logos Official Modules" }
        ]);
        waitForRendering(testCase);

        var bare = null, withOrigin = null, onGithub = null;
        tryVerify(function () {
            bare = deepFind(dlg.contentItem, "intentInstallSubtitle_probe_ui");
            withOrigin = deepFind(dlg.contentItem, "intentInstallSubtitle_other_ui");
            onGithub = deepFind(dlg.contentItem, "intentInstallSubtitle_third_ui");
            return bare !== null && withOrigin !== null && onGithub !== null;
        }, 5000, "all three subtitles are realised");

        compare(bare.text, "probe_ui", "no separator when there is no origin");

        // One line to spare per row, so the repository's name rather than its
        // link — the same name the App Manager and Settings show for it.
        compare(withOrigin.text, "other_ui · Example Packages");
        compare(onGithub.text, "third_ui · Logos Official Modules");

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
    Component { id: copySpy;    SignalSpy { signalName: "copied" } }
}
