import QtQuick

// Waits for its backend in ui-host, then calls one slot through the replica.
// tests/standalone-backend-tests.mjs asserts both texts.
Rectangle {
    id: root
    color: "white"

    readonly property var backend: logos.module("standalone_backend_fixture")
    property bool ready: false
    property string sum: "pending"

    function callBackend() {
        logos.watch(backend.add(2, 3),
                    function(value) { root.sum = String(value) },
                    function(error) { root.sum = "error " + error })
    }

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName !== "standalone_backend_fixture") return
            root.ready = isReady && root.backend !== null
            if (root.ready) root.callBackend()
        }
    }
    Component.onCompleted: {
        root.ready = root.backend !== null && logos.isViewModuleReady("standalone_backend_fixture")
        if (root.ready) root.callBackend()
    }

    Column {
        anchors.centerIn: parent
        spacing: 8
        Text { text: root.ready ? "Backend: connected" : "Backend: connecting" }
        Text { text: "Backend sum: " + root.sum }
    }
}
