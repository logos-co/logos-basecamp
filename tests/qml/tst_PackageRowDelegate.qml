import QtQuick
import QtTest

// Directory import, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"

// A row of the install gate's optional packages (ConfirmationDialog).
TestCase {
    id: testCase
    name: "PackageRowDelegate"
    when: windowShown
    width: 640
    height: 200

    Component {
        id: rowComp
        PackageRowDelegate {
            width: 560
        }
    }

    function makeRow(width, extra) {
        const appRow = {
            name: "wallet_module",
            displayName: "Wallet module",
            description: "Keys, balances and transfers for every account you hold on the network",
            action: "install",
            toVersion: "1.0.0"
        }
        for (const k in extra) appRow[k] = extra[k]
        const row = createTemporaryObject(rowComp, testCase.Window.window.contentItem, {
            width: width, selectable: true, selectionObjectName: "optional.wallet_module",
            appRow: appRow
        })
        verify(row)
        waitForRendering(row)
        return row
    }

    function columnWidths(row) {
        const name = findChild(row, "packageRow.name.wallet_module").parent
        const version = findChild(row, "packageRow.version.wallet_module").parent
        return { name: name.width, version: version.width }
    }

    // At the dialog's own width the name and version keep theirs (150, 110)
    // and the description takes the rest, however long it is.
    function test_a_desktop_row_keeps_its_columns() {
        const row = makeRow(560)
        const widths = columnWidths(row)
        compare(widths.name, 150)
        compare(widths.version, 110)
        compare(findChild(row, "packageRowDescription").opacity, 1)
        const badge = findChild(row, "packageRowStageBadge")
        compare(badge.text, "Install")
        verify(badge.mapToItem(row, badge.width, 0).x <= row.width + 0.5, "the badge stays in the row")
    }

    // In a phone-wide dialog the description goes, then the name and version
    // give way: the badge stays in the row, and the description is left out,
    // not squeezed to a lone "…".
    function test_a_phone_wide_row_gives_way_data() {
        return [{ tag: "296", width: 296 }, { tag: "326", width: 326 }]
    }

    function test_a_phone_wide_row_gives_way(data) {
        const row = makeRow(data.width)
        const badge = findChild(row, "packageRowStageBadge")
        verify(badge.mapToItem(row, badge.width, 0).x <= row.width, "the badge stays in the row")
        compare(findChild(row, "packageRowDescription").opacity, 0)
        const widths = columnWidths(row)
        verify(widths.name < 150 && widths.version < 110, JSON.stringify(widths))
    }

    // An embedded module the catalog lacks: kept as installed, nothing to pick.
    function test_an_installed_only_row_has_no_choices() {
        const row = makeRow(560, { installedVersion: "0.1.0", installedOnly: true,
                                   action: "installed", versions: [] })
        verify(!findChild(row, "optional.wallet_module").enabled, "nothing to opt out of")
        verify(!findChild(row, "packageRow.version.wallet_module").visible, "no version picker")
        compare(findChild(row, "packageRowStageBadge").text, "Installed")
    }
}
