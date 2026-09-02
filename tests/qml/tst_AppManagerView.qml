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
        // Appended, so every value above keeps its number.
        compare(InstallStage.Downloaded,  6);
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

    // ── Phase 4: the proxies are declared in QML and BOUND to the backend ───
    //
    // ContentViews.qml and OverlayDialogs.qml now declare their own
    // AppsFilterProxy and bind sourceModel / requiredPackageEntries to backend
    // properties, instead of receiving prebuilt proxies the host owned. That
    // migration compiles and runs even when the binding never fires — it just
    // renders an empty list — so these cover the wiring itself.

    function test_requiredPackageEntries_is_writable_and_drives_the_name_list() {
        var proxy = filterProxyComp.createObject(testCase);
        // Property assignment, not the Q_INVOKABLE setter: this is the path a
        // QML binding actually takes.
        proxy.requiredPackageEntries = [
            { name: "wallet_ui",     repositoryUrl: "https://repo1/" },
            { name: "wallet_module", repositoryUrl: "https://repo2/" },
        ];
        compare(proxy.requiredPackageEntries.length, 2, "entries round-trip");
        compare(proxy.requiredPackages.length, 2, "and drive the name list");
        compare(proxy.requiredPackages[0], "wallet_ui", "in resolver order");
        compare(proxy.requiredPackages[1], "wallet_module");
    }

    function test_requiredPackageEntries_binding_tracks_its_source() {
        // Stands in for `requiredPackageEntries: backend.requiredPackages`.
        var source = sourceHolderComp.createObject(testCase);
        var proxy = boundProxyComp.createObject(testCase, { holder: source });
        compare(proxy.requiredPackages.length, 0, "empty before the source is set");

        source.entries = [{ name: "extras", repositoryUrl: "" }];
        compare(proxy.requiredPackages.length, 1, "binding re-evaluated on change");
        compare(proxy.requiredPackages[0], "extras");

        source.entries = [
            { name: "extras",  repositoryUrl: "" },
            { name: "another", repositoryUrl: "" },
        ];
        compare(proxy.requiredPackages.length, 2, "and again on the next change");
    }

    function test_requiredPackageEntries_does_not_re_notify_for_an_equal_list() {
        var proxy = filterProxyComp.createObject(testCase);
        var entries = [{ name: "extras", repositoryUrl: "" }];
        proxy.requiredPackageEntries = entries;
        var spy = spyComp.createObject(testCase, {
            target: proxy, signalName: "requiredPackagesChanged",
        });
        proxy.requiredPackageEntries = entries;
        compare(spy.count, 0, "an identical re-set is dropped, so a binding cannot loop");
    }

    function test_required_packages_proxy_configuration_matches_OverlayDialogs() {
        // Mirrors the declaration in OverlayDialogs.qml. installStateFilter is
        // the one that matters: "" is NOT the default ("all"), and silently
        // reverting it would filter the Required Packages list down to nothing.
        var proxy = filterProxyComp.createObject(testCase, {
            excludeMainUi:      false,
            installStateFilter: "",
        });
        compare(proxy.excludeMainUi, false, "required-packages list includes main_ui");
        compare(proxy.installStateFilter, "", "and is not restricted by install state");
    }

    function test_ui_apps_proxy_configuration_matches_ContentViews() {
        // Mirrors the declaration in ContentViews.qml.
        var proxy = filterProxyComp.createObject(testCase, {
            typeFilter:    "ui_qml",
            excludeMainUi: true,
        });
        compare(proxy.typeFilter, "ui_qml", "App Manager shows ui_qml apps");
        compare(proxy.excludeMainUi, true, "and never basecamp's own shell");
    }

    Component { id: filterProxyComp; AppsFilterProxy {} }
    Component { id: spyComp; SignalSpy {} }

    // Stand-in for the backend object a real binding reads from.
    Component {
        id: sourceHolderComp
        QtObject { property var entries: [] }
    }

    Component {
        id: boundProxyComp
        AppsFilterProxy {
            property QtObject holder
            requiredPackageEntries: holder ? holder.entries : []
        }
    }

    property var testCase: this
}
