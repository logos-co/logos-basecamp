import QtQuick

// Intent test fixture — provider B.
//
// Identical to its sibling apart from the marker, which is the point: the pair
// is the only way to exercise the Ambiguous branch and the chooser, since no
// two shipping apps declare the same intent and the real duplicate (wallet_ui)
// cannot be co-installed.
//
// It answers on a TIMER, not synchronously. A synchronous answer completes the
// whole round trip inside one event-loop turn, so the broker's back-edge
// returns the user to the requester before the provider has been painted —
// making a correctly working system look like the chooser did nothing. A real
// provider shows UI and responds after the user acts; this models that.
Rectangle {
    id: root
    // Unique per provider so a doctest can address each one directly with
    // find_by objectName — the doctest verb set has no way to filter a list
    // of same-named matches the way ui-tests.mjs can.
    objectName: "providerRootB"
    color: "#1e1e1e"

    // Self-identifying, so a test can tell WHICH provider a view belongs to.
    // objectName alone is not enough: both providers use "providerRoot", and
    // an objectName on a child Text is not addressable as a QML id.
    property string providerName: "intent_provider_b"

    property string lastHandled: ""
    property string pendingRequestId: ""
    property var    pendingParams: ({})

    Text {
        anchors.centerIn: parent
        color: "#ffffff"
        objectName: "providerMarker"
        text: "PROVIDER_B_VIEW " + root.lastHandled
    }

    Timer {
        id: replyTimer
        interval: 1200
        repeat: false
        onTriggered: {
            root.lastHandled = "handled";
            logos.respond(root.pendingRequestId, true,
                          ({ echo: root.pendingParams.text || "",
                             provider: "intent_provider_b" }),
                          "");
            root.pendingRequestId = "";
        }
    }

    Connections {
        target: logos
        function onIntentRequested(requestId, intent, params, requesterName) {
            root.pendingRequestId = requestId;
            root.pendingParams = params;
            root.lastHandled = "working…";
            replyTimer.start();
        }
    }
}
