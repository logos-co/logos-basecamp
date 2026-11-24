import QtQuick
import QtTest
import Basecamp.Backend 1.0
import Basecamp.AppManager

// A package row in the Add Application dialog while it downloads. Where the
// bytes come from is in the badge: a caption beside it once had no width
// limit, and pushed the install button out of the dialog.
TestCase {
    id: testCase
    name: "PackageRowTransport"
    when: windowShown
    width: 640
    height: 200

    Component {
        id: rowComp
        // The dialog's width: its package list is full-bleed, so a row gets
        // all of it (AddApplicationDialog.qml).
        PackageRowDelegate {
            width: 560
        }
    }

    function makeRow(transport, received) {
        const row = createTemporaryObject(rowComp, testCase.Window.window.contentItem, {
            appRow: {
                name: "wallet_module",
                displayName: "Wallet module",
                description: "Keys, balances and transfers for every account you hold on the network",
                action: "install",
                installStage: InstallStage.Downloading,
                downloadReceived: received,
                downloadTotal: 4096,
                downloadTransport: transport,
                downloadFellBack: false
            }
        })
        verify(row)
        return row
    }

    // Everything about the download lives in the badge: the description
    // line stays the description.
    function test_the_description_stays_while_downloading() {
        const row = makeRow("logos", 0)
        compare(findChild(row, "packageRowDescription").text,
                "Keys, balances and transfers for every account you hold on the network")
    }

    function test_the_badge_starts_with_the_source_icon() {
        const row = makeRow("logos", 0)
        const badge = findChild(row, "packageRowStageBadge")
        verify(String(badge.iconSource).indexOf("logos-storage.svg") >= 0)
        compare(badge.text, "Finding peers…")
    }

    function test_the_badge_stays_inside_the_row() {
        const row = makeRow("logos", 0)
        const badge = findChild(row, "packageRowStageBadge")
        verify(badge)
        tryVerify(function() {
            return badge.mapToItem(row, badge.width, 0).x <= row.width + 0.5
        }, 1000, "the badge's right edge must stay within the row")
    }

    function test_the_description_comes_back_when_nothing_downloads() {
        const row = makeRow("", 0)
        compare(findChild(row, "packageRowDescription").text,
                "Keys, balances and transfers for every account you hold on the network")
    }

    // ── Width ──

    function makePlainRow(width) {
        const row = createTemporaryObject(rowComp, testCase.Window.window.contentItem, {
            width: width,
            appRow: {
                name: "wallet_module",
                displayName: "Wallet module",
                description: "Keys, balances and transfers for every account you hold on the network",
                action: "install",
                toVersion: "1.0.0"
            }
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
        const row = makePlainRow(560)
        const widths = columnWidths(row)
        compare(widths.name, 150)
        compare(widths.version, 110)
        compare(findChild(row, "packageRowDescription").opacity, 1)
    }

    // In a phone-wide dialog the description goes, then the name and version
    // give way: the badge stays in the row, and the description is left out,
    // not squeezed to a lone "…".
    function test_a_phone_wide_row_gives_way_data() {
        return [{ tag: "296", width: 296 }, { tag: "326", width: 326 }]
    }

    function test_a_phone_wide_row_gives_way(data) {
        const row = makePlainRow(data.width)
        const badge = findChild(row, "packageRowStageBadge")
        verify(badge.mapToItem(row, badge.width, 0).x <= row.width, "the badge stays in the row")
        compare(findChild(row, "packageRowDescription").opacity, 0)
        const widths = columnWidths(row)
        verify(widths.name < 150 && widths.version < 110, JSON.stringify(widths))
    }
}
