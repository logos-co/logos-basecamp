import QtQuick
import QtTest

import "../../src/Basecamp/Shell"

// The consent prompt for a runtime asking to pair: what it shows, what it
// answers, and when it goes away without an answer.
TestCase {
    id: testCase
    name: "PairingRequestDialog"
    when: windowShown

    Component { id: dialogComp; PairingRequestDialog {} }
    Component { id: spyComp; SignalSpy {} }

    function request(id, extra) {
        var r = { id: id, direction: "incoming", code: "418093", peer_name: "Basecamp on laptop",
                  peer_display_id: "P3V7K-9MWQD-2XT8B-RN4HC", role: "peer",
                  needs_approval: true, expires_ms: 60000 }
        for (var k in extra) r[k] = extra[k]
        return r
    }

    // A Dialog is a Popup: its content and footer are not in findChild()'s walk.
    function deepFind(node, name) {
        if (!node) return null
        if (node.objectName === name) return node
        var kids = []
        if (node.children)
            for (var i = 0; i < node.children.length; ++i) kids.push(node.children[i])
        if (node.contentItem && kids.indexOf(node.contentItem) < 0) kids.push(node.contentItem)
        for (var j = 0; j < kids.length; ++j) {
            var hit = deepFind(kids[j], name)
            if (hit) return hit
        }
        return null
    }

    function button(dlg, name) {
        var b = null
        tryVerify(function () {
            b = deepFind(dlg.footerItem, name)
            return b !== null && b.width > 0
        }, 5000, name + " is realised")
        return b
    }

    function test_shows_the_code_and_peer_strings_as_plain_text() {
        var dlg = createTemporaryObject(dialogComp, testCase)
        dlg.openWith(request("p1", { peer_name: "<b>Mallory</b>" }))
        waitForRendering(testCase)
        verify(dlg.visible)
        compare(deepFind(dlg.contentItem, "pairingRequestCode").text, "418 093")
        var body = deepFind(dlg.contentItem, "pairingRequestBody")
        compare(body.textFormat, Text.PlainText)
        verify(body.text.indexOf("<b>Mallory</b>") >= 0, body.text)
        verify(deepFind(dlg.contentItem, "pairingRequestDisplayId").text.indexOf("P3V7K-9MWQD") >= 0)
    }

    function test_each_answer_names_the_request_and_closes() {
        var dlg = createTemporaryObject(dialogComp, testCase)
        var confirmed = createTemporaryObject(spyComp, testCase, { target: dlg, signalName: "confirmRequested" })
        var rejected = createTemporaryObject(spyComp, testCase, { target: dlg, signalName: "rejectRequested" })

        dlg.openWith(request("p1"))
        mouseClick(button(dlg, "pairingRequestAccept"))
        compare(confirmed.count, 1)
        compare(confirmed.signalArguments[0][0], "p1")
        tryVerify(function () { return !dlg.visible }, 5000, "closed after accepting")

        dlg.openWith(request("p2"))
        mouseClick(button(dlg, "pairingRequestReject"))
        compare(rejected.count, 1)
        compare(rejected.signalArguments[0][0], "p2")
        compare(confirmed.count, 1, "rejecting accepts nothing")
    }

    function test_requests_wait_their_turn() {
        var dlg = createTemporaryObject(dialogComp, testCase)
        dlg.openWith(request("p1"))
        dlg.openWith(request("p2"))
        dlg.openWith(request("p2"))
        compare(dlg.request.id, "p1")
        compare(dlg.queued.length, 1, "a repeated request is not queued twice")
        dlg.close()
        tryVerify(function () { return dlg.visible && dlg.request.id === "p2" }, 5000, "the next one shows")
    }

    // The list can lag the event, so only a request once listed and then gone closes.
    function test_one_answered_elsewhere_goes_away() {
        var dlg = createTemporaryObject(dialogComp, testCase)
        dlg.openWith(request("p1"))
        dlg.syncWith([])
        verify(dlg.visible, "a list older than the request does not close it")
        dlg.syncWith([request("p1")])
        verify(dlg.visible)
        dlg.syncWith([])
        tryVerify(function () { return !dlg.visible }, 5000, "answered or expired elsewhere")
    }

    function test_an_operator_request_is_compared_by_id() {
        var dlg = createTemporaryObject(dialogComp, testCase)
        dlg.openWith(request("op", { role: "operator" }))
        waitForRendering(testCase)
        verify(!deepFind(dlg.contentItem, "pairingRequestCode").visible, "no code to compare")
        compare(button(dlg, "pairingRequestAccept").text, "Same ID, accept")
    }

    function test_it_expires() {
        var dlg = createTemporaryObject(dialogComp, testCase)
        dlg.openWith(request("p1", { expires_ms: 200 }))
        verify(dlg.visible)
        tryVerify(function () { return !dlg.visible }, 5000, "gone when the request expires")
    }
}
