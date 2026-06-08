// evil_app — an adversarial ui_qml "app" used as a sandbox-escape fixture.
//
// Where counter_qml (repos/counter_qml/Main.qml) is a benign probe panel whose
// escape attempts a human triggers by clicking buttons, this app is its evil
// twin: on load it AUTOMATICALLY fires every escape vector and tallies how many
// got through. A headless test (tests/sandbox/tst_qml_sandbox.cpp) loads this
// view through the REAL production sandbox (QmlSandbox::configure) and asserts
// `escapes === 0` — i.e. the sandbox blocked all of them.
//
// It is intentionally pure-QtQml (root QtObject, no QtQuick). The existing
// sandbox tests avoid QtQuick because its native plugin may not be resolvable in
// the offscreen pre-install test environment; staying on QtQml's builtin types
// keeps the fixture hermetic, so a non-zero `escapes` can only mean a real
// sandbox failure — never a flaky test env. Each vector below maps 1:1 to a
// counter_qml probe but uses a QtQml-global equivalent that hits the SAME
// sandbox mechanism:
//
//   counter_qml probe                      -> evil_app vector (same mechanism)
//   XMLHttpRequest GET https://…           -> httpStatus            (DenyAllNAM)
//   XMLHttpRequest GET file:///etc/hosts   -> fileXhrStatus         (DenyAllNAM)
//   Qt.openUrlExternally(file:///etc/hosts)-> openUrlStatus         (interceptUrl UrlString)
//   Loader.source = http://…/fake.qml      -> remoteCompStatus      (interceptor blocks remote scheme)
//   Image.source = file:///etc/hosts       -> outsideFileCompStatus (interceptor blocks file outside roots)
//
// The counter_qml `QFile` probe is deliberately omitted: QFile is not a
// registered QML type (basecamp exposes only the `logos` bridge + `isActiveTab`
// to plugin engines), so it is absent by construction, not a sandbox guarantee.
//
// Each probe writes a human-readable "<name>: BLOCKED (...)" / "ESCAPED (...)"
// status string AND increments `escapes` on escape. `probesDone` flips true once
// every async probe has reported, which is the test's cue to read the tally.
import QtQml

QtObject {
    id: root

    // --- Per-probe human-readable status (also handy in a manual basecamp load) ---
    property string httpStatus: "pending"
    property string fileXhrStatus: "pending"
    property string openUrlStatus: "pending"
    property string remoteCompStatus: "pending"
    property string outsideFileCompStatus: "pending"

    // --- Tally the test reads ---
    property int escapes: 0          // how many vectors were NOT blocked
    property int probesReported: 0   // how many probes have finished
    readonly property int probeCount: 5
    property bool probesDone: probesReported >= probeCount

    // Banner string a human sees if this is hand-loaded into basecamp.
    readonly property string summary:
        probesDone ? ("ESCAPES: " + escapes + " / " + probeCount)
                   : ("running probes… " + probesReported + "/" + probeCount)

    // Tracks which probes have already reported, so report() is idempotent — an
    // async callback and the watchdog can both fire for the same probe, but only
    // the first counts. This is what lets the watchdog safely finalize any probe
    // whose callback never arrived.
    property var _seen: ({})

    // Record one probe's outcome. `escaped` true means the sandbox let it through.
    function report(name, escaped, detail) {
        if (root._seen[name])
            return;
        root._seen[name] = true;
        if (escaped)
            root.escapes += 1;
        root.probesReported += 1;
        const verdict = (escaped ? "ESCAPED" : "BLOCKED") + (detail ? " (" + detail + ")" : "");
        switch (name) {
            case "http":        root.httpStatus = verdict; break;
            case "fileXhr":     root.fileXhrStatus = verdict; break;
            case "openUrl":     root.openUrlStatus = verdict; break;
            case "remoteComp":  root.remoteCompStatus = verdict; break;
            case "outsideFile": root.outsideFileCompStatus = verdict; break;
        }
    }

    // XMLHttpRequest goes through the engine's QNetworkAccessManager, which the
    // sandbox replaces with a deny-all NAM. A blocked request surfaces as status
    // 0 (never reached the server). Anything else (a real 2xx/4xx from a server)
    // means the network was actually reachable -> escape. Note: QML additionally
    // refuses file:// GETs by policy (a warning, no DONE event), so the file probe
    // may simply never call back — the watchdog finalizes it as blocked, which is
    // the correct verdict (nothing was ever read).
    function probeXhr(name, url) {
        const xhr = new XMLHttpRequest();
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE)
                root.report(name, xhr.status !== 0, "status=" + xhr.status);
        };
        xhr.onerror = function() { root.report(name, false, "error"); };
        try {
            xhr.open("GET", url, true);
            xhr.send();
        } catch (e) {
            // Synchronous throw == request refused == blocked.
            root.report(name, false, "threw");
        }
    }

    // Drives one Qt.createComponent() and reports Error/Null as blocked, Ready as
    // an escape. Used for the remote-scheme and out-of-root-file vectors: both
    // must be refused by the URL interceptor (empty QUrl -> not Ready).
    property var _components: []
    function probeComponent(name, url) {
        const comp = Qt.createComponent(url, Component.Asynchronous);
        root._components.push(comp); // keep a ref so it isn't GC'd mid-load
        function settle() {
            if (comp.status === Component.Loading)
                return;
            // Ready == the engine resolved and compiled the URL == escape.
            root.report(name, comp.status === Component.Ready, "status=" + comp.status);
        }
        comp.statusChanged.connect(settle);
        settle(); // in case it resolved synchronously (blocked URLs often do)
    }

    function runProbes() {
        // Network: HTTP and file:// both ride the deny-all NAM.
        probeXhr("http", "https://example.com");
        probeXhr("fileXhr", "file:///etc/hosts");

        // Qt.openUrlExternally resolves through QtObject::resolvedUrl ->
        // engine->interceptUrl(UrlString); the sandbox interceptor returns an
        // empty URL for a path outside the module roots, so the call returns
        // false (nothing opened). true == the host was asked to open it == escape.
        var opened = false;
        try {
            opened = Qt.openUrlExternally("file:///etc/hosts");
        } catch (e) {
            opened = false;
        }
        report("openUrl", opened === true, "returned=" + opened);

        // Remote-scheme component load (counter_qml's Loader http probe).
        probeComponent("remoteComp", "http://example.com/fake.qml");
        // Local file outside the sandbox roots (counter_qml's Image file probe).
        probeComponent("outsideFile", "file:///etc/hosts");

        // Watchdog: finalize any probe that produced no completion signal. A probe
        // that never reports did NOT reach the network/file (an escape would emit a
        // concrete success), so "no completion" == blocked. Without this, one
        // silently-refused async probe (notably the file:// XHR) would leave
        // probesDone stuck false forever.
        root._watchdog.start();
    }

    // A 0ms timer defers probing until after the component is fully constructed
    // and the engine is idle, so createComponent/XHR callbacks fire cleanly.
    property var _kick: Timer {
        interval: 0
        running: true
        repeat: false
        onTriggered: root.runProbes()
    }

    property var _watchdog: Timer {
        interval: 1500
        repeat: false
        onTriggered: {
            root.report("http", false, "no completion");
            root.report("fileXhr", false, "no completion");
            root.report("openUrl", false, "no completion");
            root.report("remoteComp", false, "no completion");
            root.report("outsideFile", false, "no completion");
        }
    }
}
