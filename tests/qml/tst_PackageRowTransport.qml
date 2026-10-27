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
}
