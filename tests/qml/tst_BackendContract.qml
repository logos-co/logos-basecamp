import QtQuick
import QtTest
import Basecamp.Backend
import BasecampTests

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

    function test_AppsFilterProxy_QML_instantiable_with_defaults() {
        var proxy = filterProxyComp.createObject(testCase);
        verify(proxy, "filter proxy created from QML");
        compare(proxy.typeFilter, "", "type filter empty by default");
        compare(proxy.searchText, "", "search empty by default");
        compare(proxy.excludeMainUi, true, "main_ui excluded by default");
        compare(proxy.visibleCount, 0, "no rows without a source model");
    }

    // The welcome page's proxies, bound to backend.appsModel: what each shows.
    function test_the_welcome_pages_proxies_filter_by_type_and_search() {
        var model = modelComp.createObject(testCase);
        model.setCatalog([
            { name: "main_ui", type: "ui_qml", repositoryUrl: "https://repo/", versions: [] },
            { name: "wallet_ui", displayName: "Wallet", type: "ui_qml", repositoryUrl: "https://repo/", versions: [] },
            { name: "chat_ui", displayName: "Chat", type: "ui_qml", repositoryUrl: "https://repo/", versions: [] },
            { name: "wallet_module", description: "Keys for the wallet", type: "core",
              repositoryUrl: "https://repo/", versions: [] },
        ]);
        var apps = filterProxyComp.createObject(testCase, { sourceModel: model, typeFilter: "ui_qml" });
        var packages = filterProxyComp.createObject(testCase, { sourceModel: model, typeFilter: "core" });
        compare(apps.visibleCount, 2, "apps, never Basecamp's own shell");
        compare(packages.visibleCount, 1);
        apps.searchText = "wall";
        packages.searchText = "wall";
        compare(apps.visibleCount, 1, "by display name");
        compare(packages.visibleCount, 1, "and by description");
    }

    Component { id: filterProxyComp; AppsFilterProxy {} }
    Component { id: modelComp; TestAppsModel {} }

    property var testCase: this
}
