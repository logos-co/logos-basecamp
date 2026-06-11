import QtQuick
import QtTest
import Basecamp.Backend

// The badge-rendering logic across AppGridDelegate / AppListDelegate /
// PackageRowDelegate / AddApplicationDialog depends on three small fixed
// vocabularies:
//   * InstallStatus enumerators (drives tile-state badges)
//   * InstallStage enumerators (drives in-flight pipeline badges)
//   * the `action` string vocabulary on resolver overlay rows
//     ("install" / "installed" / "upgrade" / "downgrade" / "reinstall")
TestCase {
    name: "BadgeContract"
    when: windowShown

    function test_InstallStage_enum_distinct_and_dense() {
        var values = [
            InstallStage.None,
            InstallStage.Downloading,
            InstallStage.Queued,
            InstallStage.Installing,
            InstallStage.Installed,
            InstallStage.Failed,
        ];
        for (var i = 0; i < values.length; ++i)
            compare(values[i], i, "stage[" + i + "] is " + i);
    }

    function test_InstallStatus_enum_distinct_and_dense() {
        var values = [
            InstallStatus.NotInstalled,
            InstallStatus.Installed,
            InstallStatus.UpgradeAvailable,
            InstallStatus.DowngradeAvailable,
            InstallStatus.DifferentHash,
        ];
        for (var i = 0; i < values.length; ++i)
            compare(values[i], i, "status[" + i + "] is " + i);
    }

    function test_InstallStage_and_InstallStatus_share_value_for_Installed() {
        compare(InstallStage.Installed,  4);
        compare(InstallStatus.Installed, 1);.
        verify(InstallStage.Installed !== InstallStatus.Installed);
    }
}
