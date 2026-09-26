import QtQuick
import QtTest

// Directory import, as for the Shell tests: Settings is compiled into main_ui.
import "../../src/Basecamp/Settings"

// Settings -> Peering against a fixed state: what it lists, and what it asks for.
TestCase {
    id: testCase
    name: "PeeringView"
    when: windowShown
    // A TestCase is invisible by default, and so would be everything in it.
    visible: true
    width: 900
    height: 1600

    readonly property string peerId: "3f2c1a9e-7b4d-4e8a-9c21-5d6e7f809a1b"

    Component { id: viewComp; PeeringView { width: 900; height: 1600 } }
    Component { id: spyComp; SignalSpy {} }

    function state(extra) {
        var s = {
            enabled: true, restartRequired: false, running: true, error: "",
            status: { name: "Basecamp on desk", display_id: "K7Q2M-4XWPD-9TR3B-HN6ZC" },
            peers: [{ runtime_id: peerId, alias: "node", display_name: "node",
                      display_id: "B4N8R-2KDQW-7XM3P-LT9VH", status: "active" }],
            pending: [], imports: [], localInvitePath: "/home/me/.logosctl/peering/local-invite",
            localInviteFound: true
        }
        for (var k in extra) s[k] = extra[k]
        return s
    }

    function view(extra) {
        var v = createTemporaryObject(viewComp, testCase, { peering: state(extra) })
        verify(v, "view created")
        return v
    }

    function spy(target, signalName) {
        return createTemporaryObject(spyComp, testCase, { target: target, signalName: signalName })
    }

    function test_a_finished_pairing_is_not_pending() {
        var v = view({ pending: [
            { id: "done", direction: "outgoing", code: "", state: "paired" },
            { id: "waiting", direction: "incoming", code: "123456", needs_approval: true } ] })
        verify(!findChild(v, "peering.pending.done"), "a paired pairing shows as a peer")
        verify(findChild(v, "peering.pending.waiting"), "one waiting for approval is listed")
        verify(findChild(v, "peering.pending.accept.waiting").visible)
    }

    function test_accepting_names_the_pairing() {
        var v = view({ pending: [{ id: "p1", direction: "incoming", code: "123456", needs_approval: true }] })
        var confirmed = spy(v, "confirmRequested")
        findChild(v, "peering.pending.accept.p1").clicked()
        compare(confirmed.count, 1)
        compare(confirmed.signalArguments[0][0], "p1")
    }

    function test_an_import_shows_its_state() {
        var v = view({ imports: [{ name: "monerod_module", from: peerId, module: "monerod_module",
                                   peer_alias: "node", state: "error", reason: "the peer does not answer" }] })
        compare(findChild(v, "peering.importState.monerod_module").text, "error")
    }

    function test_a_shared_module_is_imported_once() {
        var v = view({ imports: [{ name: "a_module", from: peerId, module: "a_module", state: "ready" }] })
        v.showPeerExports(peerId, [{ module: "a_module", events: true, loaded: true },
                                   { module: "b_module", events: false, loaded: true }])
        verify(!findChild(v, "peering.import." + peerId + ".a_module").enabled, "already imported")
        var imported = spy(v, "importRequested")
        findChild(v, "peering.import." + peerId + ".b_module").clicked()
        compare(imported.count, 1)
        compare(imported.signalArguments[0][0], peerId)
        compare(imported.signalArguments[0][1], "b_module")
        compare(imported.signalArguments[0][2], false)
    }

    function test_the_pairing_window_is_offered_while_control_listens() {
        var v = view({ status: { name: "Basecamp on desk", control: { enabled: true, port: 7443 },
                                 pairing_window_ms: 0 } })
        var windows = spy(v, "pairingWindowRequested")
        var button = findChild(v, "peering.pairingWindowButton")
        verify(button.visible)
        verify(findChild(v, "peering.controlState").text.indexOf("7443") >= 0)
        button.clicked()
        compare(windows.signalArguments[0][0], 300)

        var open = view({ status: { control: { enabled: true, port: 7443 }, pairing_window_ms: 120000 } })
        var closing = spy(open, "pairingWindowRequested")
        findChild(open, "peering.pairingWindowButton").clicked()
        compare(closing.signalArguments[0][0], 0, "an open window can be closed")
    }

    function test_a_control_endpoint_that_cannot_listen_says_why() {
        var v = view({ status: { control: { enabled: true, port: 0,
                                            error: "cannot listen on 0.0.0.0:17443" } } })
        verify(findChild(v, "peering.controlState").text.indexOf("cannot listen on 0.0.0.0:17443") >= 0)
        verify(!findChild(v, "peering.pairingWindowButton").visible, "no window to open")
        verify(!findChild(view({}), "peering.pairingWindowButton").visible, "nor with control off")
    }

    function test_off_offers_only_the_switch() {
        var v = view({ enabled: false, running: false, peers: [] })
        verify(findChild(v, "peering.enabledSwitch").visible)
        verify(!findChild(v, "peering.linkLocalButton").visible, "nothing to link with while off")
        verify(!findChild(v, "peering.pairButton").visible)
    }
}
