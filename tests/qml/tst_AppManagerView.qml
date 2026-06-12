import QtQuick
import QtTest
import Basecamp.Backend

TestCase {
    name: "BackendContract"
    when: windowShown

    function test_InstallStatus_enum_values_stable() {
        compare(InstallStatus.NotInstalled,        0);
        compare(InstallStatus.Installed,           1);
        compare(InstallStatus.UpgradeAvailable,    2);
        compare(InstallStatus.DowngradeAvailable,  3);
        compare(InstallStatus.DifferentHash,       4);
    }

    function test_InstallStage_enum_values_stable() {
        compare(InstallStage.None,        0);
        compare(InstallStage.Downloading, 1);
        compare(InstallStage.Queued,      2);
        compare(InstallStage.Installing,  3);
        compare(InstallStage.Installed,   4);
        compare(InstallStage.Failed,      5);
    }

    function test_AppsFilterProxy_QML_instantiable_with_defaults() {
        var proxy = filterProxyComp.createObject(testCase);
        verify(proxy, "filter proxy created from QML");
        compare(proxy.typeFilter, "", "type filter empty by default");
        compare(proxy.installStateFilter, "all", "install-state defaults to all");
        compare(proxy.excludeMainUi, true, "main_ui excluded by default");
        compare(proxy.repositoryUrlFilter, "", "repo filter empty by default");
        compare(proxy.visibleCount, 0, "no rows without a source model");
        compare(proxy.requiredPackages.length, 0, "no required packages");
    }

    function test_AppsFilterProxy_repositoryUrlFilter_setter_emits_change() {
        var proxy = filterProxyComp.createObject(testCase);
        var spy = spyComp.createObject(testCase, {
            target: proxy, signalName: "repositoryUrlFilterChanged",
        });
        proxy.repositoryUrlFilter = "https://example/repo.json";
        compare(spy.count, 1, "change notify fires on set");
        compare(proxy.repositoryUrlFilter, "https://example/repo.json");
        // Idempotent — no extra fire if value is unchanged.
        proxy.repositoryUrlFilter = "https://example/repo.json";
        compare(spy.count, 1, "no spurious re-emit");
    }

    Component { id: filterProxyComp; AppsFilterProxy {} }
    Component { id: spyComp; SignalSpy {} }

    property var testCase: this
}
