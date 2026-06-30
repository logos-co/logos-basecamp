import QtQuick
import QtTest
import Basecamp.Backend

TestCase {
    name: "RepositoryContract"
    when: windowShown

    function test_ModuleFilterProxy_requiredPackages_preserves_insertion_order() {
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

    function test_ModuleFilterProxy_filters_chain_through() {
        var outer = filterProxyComp.createObject(testCase);
        var inner = filterProxyComp.createObject(testCase, {
            sourceModel: outer,
            repositoryUrlFilter: "https://example/repo.json",
        });
        compare(outer.visibleCount, 0, "outer empty without source");
        compare(inner.visibleCount, 0, "chained inner empty too");
        compare(inner.repositoryUrlFilter, "https://example/repo.json");
    }

    Component { id: filterProxyComp; ModuleFilterProxy {} }

    property var testCase: this
}
