import QtQuick
import QtTest
import Basecamp.Backend

// Directory import, not `import Basecamp.Settings` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Settings"

TestCase {
    name: "RepositoryContract"
    when: windowShown
    // A TestCase starts hidden; the view takes clicks only if this shows.
    visible: true
    width: 900
    height: 700

    function test_AppsFilterProxy_requiredPackages_preserves_insertion_order() {
        var proxy = filterProxyComp.createObject(testCase);
        proxy.setRequiredPackages([
            { name: "wallet_ui",     repositoryUrl: "https://repo1/" },
            { name: "wallet_module", repositoryUrl: "https://repo2/" },
            { name: "extras",        repositoryUrl: "" },
        ]);
        compare(proxy.requiredPackages.length, 3);
        compare(proxy.requiredPackages[0], "wallet_ui");
        compare(proxy.requiredPackages[1], "wallet_module");
        compare(proxy.requiredPackages[2], "extras");
    }

    function test_AppsFilterProxy_filters_chain_through() {
        var outer = filterProxyComp.createObject(testCase);
        var inner = filterProxyComp.createObject(testCase, {
            sourceModel: outer,
            repositoryUrlFilter: "https://example/repo.json",
        });
        compare(outer.visibleCount, 0, "outer empty without source");
        compare(inner.visibleCount, 0, "chained inner empty too");
        compare(inner.repositoryUrlFilter, "https://example/repo.json");
    }

    // A row's switch and trash button share its top line. The trash button
    // removes a repository at once, but asks first for the default one.
    function test_a_rows_trash_button() {
        var view = createTemporaryObject(viewComp, testCase, { repositories: [
            { url: "https://default/", displayName: "Logos", isDefault: true, enabled: true },
            { url: "https://added/", displayName: "Added", description: "Someone's packages", enabled: false }] })
        removeSpy.target = view
        removeSpy.clear()
        tryVerify(function() { return !!findChild(view, "repositories.remove.https://added/") })
        waitForRendering(view)

        var toggle = findChild(view, "repositories.enableSwitch.https://added/")
        var trash = findChild(view, "repositories.remove.https://added/")
        compare(trash.parent, toggle.parent, "on the switch's line")
        verify(trash.x > toggle.x, "after the switch")
        mouseClick(trash)
        compare(removeSpy.count, 1)
        compare(removeSpy.signalArguments[0][0], "https://added/")

        mouseClick(findChild(view, "repositories.remove.https://default/"))
        var cancel = findChild(view, "repositories.removeConfirm.cancel")
        tryVerify(function() { return cancel.visible })
        compare(removeSpy.count, 1, "the default is not removed without asking")
        // Until laid out, the dialog's buttons sit on top of each other.
        waitForRendering(testCase)
        mouseClick(cancel)
        compare(removeSpy.count, 1)
    }

    Component { id: filterProxyComp; AppsFilterProxy {} }
    Component { id: viewComp; RepositoriesView { width: 760; height: 600 } }
    SignalSpy { id: removeSpy; signalName: "removeRequested" }

    property var testCase: this
}
