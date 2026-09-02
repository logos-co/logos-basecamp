import QtQuick

// Intent test fixture — the requester.
//
// Declares `uses` for test.echo only. The undeclared button is deliberate: an
// app asking for something absent from its own manifest must get not_declared,
// and that is the one error the broker answers immediately rather than holding
// to the timing floor.
Rectangle {
    id: root
    objectName: "requesterRoot"
    color: "#1e1e1e"

    // Every assertion in the integration test reads this. It holds the error
    // code on failure and "ok:<provider>" on success, so one property
    // distinguishes every outcome the frozen surface can produce.
    property string lastResult: ""

    function request(intent) {
        root.lastResult = "";
        logos.request(intent, ({ text: "hello" }), function (res) {
            root.lastResult = res.ok
                ? "ok:" + (res.data ? res.data.provider : "?")
                : res.error;
        });
    }

    // Same call, deliberately wrong payload: test.solo declares `text` as a
    // string. Nothing here is malformed as QML — it is well-formed data of the
    // wrong type, which is the case only the provider's declaration can catch.
    function requestBadParams(intent) {
        root.lastResult = "";
        logos.request(intent, ({ text: 42 }), function (res) {
            root.lastResult = res.ok
                ? "ok:" + (res.data ? res.data.provider : "?")
                : res.error;
        });
    }

    // ── Round-trip fidelity ────────────────────────────────────────────────
    //
    // One payload covering everything isCanonicalPayload() permits, sent to a
    // provider that echoes it back verbatim. What is being tested is not the
    // intent mechanism but the TRANSPORT: params cross from this app's QML
    // engine into C++, through the broker, into a second engine, and the reply
    // makes the same trip back. Anything lost or coerced on that journey shows
    // up as a mismatch here.
    //
    // Deliberately excluded: functions and QML objects (refused by design, and
    // covered by the bad_request cases), and null/undefined, whose JS-to-QVariant
    // mapping is ambiguous enough that a mismatch would say more about the test
    // than the transport.
    function roundTripPayload() {
        return {
            str:        "hello",
            emptyStr:   "",
            unicode:    "héllo ✓ 世界",
            quoted:     "he said \"hi\"\nnewline\ttab",
            intPos:     42,
            intNeg:     -7,
            zero:       0,
            big:        9007199254740991,          // 2^53 - 1, the documented max
            dbl:        12.5,
            dblNeg:     -0.125,
            boolT:      true,
            boolF:      false,
            emptyArr:   [],
            numArr:     [1, 2, 3],
            strArr:     ["a", "b"],
            mixedArr:   [1, "two", true, 3.5],
            objArr:     [{ k: 1 }, { k: 2 }],
            emptyObj:   ({}),
            nested:     { a: { b: { c: [1, { d: "deep" }] } } }
        };
    }

    // A native JS array, or the array-like Qt sequence one becomes after
    // crossing an engine boundary. Both index and report length identically.
    function isArrayLike(v) {
        return Array.isArray(v)
            || (v !== null && typeof v === "object" && typeof v.length === "number"
                && !(v instanceof Date));
    }

    // Structural comparison. Returns "" when equal, else the first path that
    // differs — a boolean would tell us something broke without saying what.
    function deepDiff(a, b, path) {
        path = path || "$";
        if (typeof a !== typeof b)
            return path + ": type " + (typeof a) + " -> " + (typeof b);

        // ARRAY-LIKENESS, NOT Array.isArray(). A list that crosses the boundary
        // arrives as a Qt sequence: length, indexing, map, forEach and
        // JSON.stringify all behave, but Array.isArray() is false — for every
        // array, not just empty ones. Asserting the prototype identity would be
        // testing a property the transport never carried, and would fail on
        // data that arrived perfectly intact.
        var aIsArr = isArrayLike(a), bIsArr = isArrayLike(b);
        if (aIsArr !== bIsArr)
            return path + ": array-ness changed — sent "
                 + (aIsArr ? "array" : typeof a) + "(" + JSON.stringify(a)
                 + "), got " + (bIsArr ? "array" : typeof b)
                 + "(" + JSON.stringify(b) + ")";
        if (aIsArr) {
            if (a.length !== b.length)
                return path + ": length " + a.length + " -> " + b.length;
            for (var i = 0; i < a.length; ++i) {
                var d = deepDiff(a[i], b[i], path + "[" + i + "]");
                if (d) return d;
            }
            return "";
        }
        if (a !== null && typeof a === "object") {
            var ka = Object.keys(a).sort(), kb = Object.keys(b).sort();
            if (ka.join(",") !== kb.join(","))
                return path + ": keys [" + ka + "] -> [" + kb + "]";
            for (var j = 0; j < ka.length; ++j) {
                var d2 = deepDiff(a[ka[j]], b[ka[j]], path + "." + ka[j]);
                if (d2) return d2;
            }
            return "";
        }
        return a === b ? "" : path + ": " + JSON.stringify(a) + " -> " + JSON.stringify(b);
    }

    function requestRoundTrip() {
        root.lastResult = "";
        var sent = roundTripPayload();
        logos.request("test.roundtrip", sent, function (res) {
            if (!res.ok)             { root.lastResult = res.error; return; }
            if (!res.data)           { root.lastResult = "no-data"; return; }
            if (!res.data.echo)      { root.lastResult = "no-echo"; return; }
            var diff = deepDiff(sent, res.data.echo);
            root.lastResult = diff === "" ? "roundtrip:ok" : "roundtrip:MISMATCH " + diff;
        });
    }

    Column {
        anchors.centerIn: parent
        spacing: 8

        Text {
            objectName: "requesterResult"
            color: "#ffffff"
            text: "RESULT " + root.lastResult
        }

        // Declared in metadata.json — resolves to one or both providers.
        Rectangle {
            objectName: "btnEcho"
            width: 200; height: 32; color: "#333333"
            Text { anchors.centerIn: parent; color: "#ffffff"; text: "Echo (declared)" }
            MouseArea { anchors.fill: parent; onClicked: root.request("test.echo") }
        }

        // Declared, and provided by EXACTLY ONE installed app. Exercises the
        // single-provider path, which still confirms — one provider is not a
        // reason to skip the user, it is the case that used to be silent.
        Rectangle {
            objectName: "btnSolo"
            width: 200; height: 32; color: "#333333"
            Text { anchors.centerIn: parent; color: "#ffffff"; text: "Solo (one provider)" }
            MouseArea { anchors.fill: parent; onClicked: root.request("test.solo") }
        }

        // Declared and provided, but sent with `text` as a number where the
        // provider's metadata.json says string. Must yield bad_request, and the
        // provider must never see it.
        Rectangle {
            objectName: "btnSoloBadParams"
            width: 200; height: 32; color: "#333333"
            Text { anchors.centerIn: parent; color: "#ffffff"; text: "Solo (bad params)" }
            MouseArea { anchors.fill: parent; onClicked: root.requestBadParams("test.solo") }
        }

        // Sends every permitted data shape and compares what comes back.
        Rectangle {
            objectName: "btnRoundTrip"
            width: 200; height: 32; color: "#333333"
            Text { anchors.centerIn: parent; color: "#ffffff"; text: "Round-trip (all types)" }
            MouseArea { anchors.fill: parent; onClicked: root.requestRoundTrip() }
        }

        // ── Auto-return, drivable by hand ──────────────────────────────
        //
        // Both go to intent_provider_manual, which is the only provider of
        // either. Same `ok:true` comes back from both; only the provider's
        // "handoff" declaration differs, and that is what decides whether the
        // shell brings you back here or leaves you there.
        Rectangle {
            objectName: "btnManual"
            width: 200; height: 32; color: "#333333"
            Text { anchors.centerIn: parent; color: "#ffffff"; text: "Manual (returns here)" }
            MouseArea { anchors.fill: parent; onClicked: root.request("test.manual") }
        }

        Rectangle {
            objectName: "btnHandoff"
            width: 200; height: 32; color: "#333333"
            Text { anchors.centerIn: parent; color: "#ffffff"; text: "Hand-off (stays there)" }
            MouseArea { anchors.fill: parent; onClicked: root.request("test.handoff") }
        }

        // NOT in metadata.json — must yield not_declared.
        Rectangle {
            objectName: "btnUndeclared"
            width: 200; height: 32; color: "#333333"
            Text { anchors.centerIn: parent; color: "#ffffff"; text: "Echo (undeclared)" }
            MouseArea { anchors.fill: parent; onClicked: root.request("test.undeclared") }
        }
    }
}
