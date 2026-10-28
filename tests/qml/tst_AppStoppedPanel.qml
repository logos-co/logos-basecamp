import QtQuick
import QtTest

// Directory import, not `import Basecamp.Shell` — see tst_IntentInstallDialog.
import "../../src/Basecamp/Shell"

// The Package Manager page is embedded and cannot be closed: its stopped
// panel offers Reopen only. A docked app's offers both.
TestCase {
    id: testCase
    name: "AppStoppedPanel"
    when: windowShown
    width: 600
    height: 400

    Component {
        id: panelComp
        AppStoppedPanel { width: 600; height: 400; appId: "some_app"; appName: "Some App" }
    }

    function test_docked_app_offers_close_and_reopen() {
        var panel = createTemporaryObject(panelComp, testCase.Window.window.contentItem)
        verify(findChild(panel, "appStopped.close").visible, "Close shown")
        verify(findChild(panel, "appStopped.reopen").visible, "Reopen shown")
    }

    function test_embedded_page_offers_reopen_only() {
        var panel = createTemporaryObject(panelComp, testCase.Window.window.contentItem,
                                         { closable: false })
        verify(!findChild(panel, "appStopped.close").visible, "Close hidden")
        verify(findChild(panel, "appStopped.reopen").visible, "Reopen shown")
    }
}
