import QtQuick

// Intent test fixture — the manual provider.
//
// The other two fixtures answer on a timer, which is enough to prove routing
// but hides the thing auto-return is actually about: WHEN the user acts. This
// one holds the request open until a button is pressed, so the return trip can
// be watched by hand rather than inferred from a property flipping.
//
// It provides one intent of each shape, and the pair is the point:
//
//   test.manual  — a transaction. You press a button when the work is done,
//                  it answers, and the shell brings you back to the requester.
//   test.handoff — the same, declaring "handoff": true. Identical button,
//                  identical `ok:true` — and the shell leaves you here.
//
// BOTH hold the request open until you press something. When a provider
// answers and whether the shell then moves you are independent: the first is
// the provider's business, the second is all `handoff` controls. Answering on
// arrival is reasonable for an intent with no completion at all (the shell's
// own repositories page works that way), but it is not what the flag means,
// and modelling it that way here hid the distinction behind two dead buttons.
Rectangle {
    id: root
    objectName: "providerRootManual"
    color: "#1e1e1e"

    property string providerName: "intent_provider_manual"

    // "waiting" while a request is open, then one of "handled" / "handed-off"
    // / "cancelled". The UI test reads this to know the request arrived before
    // pressing anything, and which shape completed.
    property string lastHandled: ""
    property string pendingRequestId: ""
    property bool   pendingIsHandoff: false

    // Who asked. HOST-ATTESTED — the shell fills this from the module name it
    // loaded the requester under, not from anything in the payload, so it is
    // the one identity here a caller cannot forge. Kept after the request ends
    // so the completed states can still name it.
    property string lastRequester: ""
    property string pendingIntent: ""

    function answer(ok, error) {
        if (root.pendingRequestId === "") return;
        root.lastHandled = !ok ? "cancelled"
                               : (root.pendingIsHandoff ? "handed-off" : "handled");
        logos.respond(root.pendingRequestId,
                      ok,
                      ok ? ({ provider: "intent_provider_manual" }) : ({}),
                      error);
        root.pendingRequestId = "";
        root.pendingIsHandoff = false;
    }

    // The buttons are dead unless a request is actually open, so a stray click
    // cannot manufacture a response for a dispatch that never happened.
    readonly property bool awaitingAnswer: root.pendingRequestId !== ""

    readonly property string statusLine: {
        if (root.awaitingAnswer) {
            return root.pendingIsHandoff
                ? "Holding a HAND-OFF request. Press Complete when the action is "
                + "really done — the requester gets its answer and you STAY here, "
                + "because the intent declared \"handoff\": true."
                : "Holding a request. Press Complete when the action is done — the "
                + "requester gets its answer and the shell brings you BACK to it."
        }
        switch (root.lastHandled) {
        case "handed-off":
            return "Completed as a hand-off. The requester has its ok and you are "
                 + "still here — that is the whole difference.\n\n"
                 + "To see what it received, open Intent Requester from the sidebar."
        case "handled":
            return "Completed. You should have been returned to the requester."
        case "cancelled":
            return "Cancelled. Where you are now depends on which intent this was: "
                 + "a hand-off leaves you here, a transaction takes you back."
        default:
            return "Idle. Ask for test.manual or test.handoff from the requester."
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 8

        Text {
            objectName: "providerMarker"
            color: "#ffffff"
            text: "PROVIDER_MANUAL_VIEW " + root.lastHandled
        }

        Text {
            objectName: "providerRequesterLine"
            width: 340
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            color: "#8fb8d8"
            visible: root.lastRequester !== ""
            text: root.pendingIntent + "  ←  requested by  " + root.lastRequester
        }

        Text {
            width: 340
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            color: root.awaitingAnswer ? "#e0e0e0" : "#9a9a9a"
            text: root.statusLine
        }

        Rectangle {
            objectName: "btnComplete"
            width: 200; height: 32
            color: root.awaitingAnswer ? "#2f5d3a" : "#333333"
            Text {
                anchors.centerIn: parent
                color: root.awaitingAnswer ? "#ffffff" : "#777777"
                text: root.pendingIsHandoff ? "Complete (stay here)"
                                            : "Complete (go back)"
            }
            MouseArea {
                anchors.fill: parent
                enabled: root.awaitingAnswer
                onClicked: root.answer(true, "")
            }
        }

        Rectangle {
            objectName: "btnCancel"
            width: 200; height: 32
            color: root.awaitingAnswer ? "#5d2f2f" : "#333333"
            Text {
                anchors.centerIn: parent
                color: root.awaitingAnswer ? "#ffffff" : "#777777"
                text: "Cancel request"
            }
            MouseArea {
                anchors.fill: parent
                enabled: root.awaitingAnswer
                onClicked: root.answer(false, "cancelled")
            }
        }
    }

    Connections {
        target: logos
        function onIntentRequested(requestId, intent, params, requesterName) {
            // Both shapes wait for the user. The only thing read off the intent
            // is which one it is, so the banner and the button can say what
            // pressing Complete will do — the shell decides the rest from the
            // provider's own `handoff` declaration, not from anything here.
            //
            // Worth knowing when holding a request open for real work: a
            // provider that accepted has 10 minutes before the backstop reports
            // `timeout` to the requester. Fine for a button press, not for
            // something waiting on a network or a chain.
            root.pendingRequestId = requestId;
            root.pendingIsHandoff = (intent === "test.handoff");
            root.lastRequester = requesterName;
            root.pendingIntent = intent;
            root.lastHandled = "waiting";
        }
    }
}
