import QtQuick

// Waits for its backend (a core module), calls one slot through the replica,
// and checks that the core itself is out of reach.
// tests/standalone-module-backend-tests.mjs asserts the texts.
Rectangle {
    id: root
    objectName: "fixture.root"
    color: "white"

    readonly property var backend: logos.module("module_backend_fixture")
    property bool ready: false
    property string sum: "pending"
    property string coreAccess: "pending"

    function callBackend() {
        logos.watch(backend.add(2, 3),
                    function(value) { root.sum = String(value) },
                    function(error) { root.sum = "error " + error })
    }

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName !== "module_backend_fixture") return
            root.ready = isReady && root.backend !== null
            if (root.ready) root.callBackend()
        }
    }
    Component.onCompleted: {
        root.ready = root.backend !== null && logos.isViewModuleReady("module_backend_fixture")
        if (root.ready) root.callBackend()
        logos.callModuleAsync("package_manager", "getInstalledPackages", [], function(payload) {
            let parsed = null
            try { parsed = JSON.parse(payload) } catch (e) {}
            // A successful call can answer any JSON value; only an object can carry an error.
            const failed = parsed === null || (typeof parsed === "object" && "error" in parsed && !("result" in parsed))
            const reached = !failed
            root.coreAccess = reached ? "REACHED " + String(payload).slice(0, 80) : "none"
        }, 10000)
    }

    Column {
        anchors.centerIn: parent
        spacing: 8
        Text { text: root.ready ? "Backend: connected" : "Backend: connecting" }
        Text { text: "Backend sum: " + root.sum }
        Text { text: "Core access: " + root.coreAccess }
    }
}
