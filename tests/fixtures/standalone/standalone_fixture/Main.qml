import QtQuick

// Renders, then makes one call through the bridge to a core module it depends
// on. tests/standalone-tests.mjs asserts both texts.
Rectangle {
    color: "white"

    property string callResult: "Bridge call: pending"

    Column {
        anchors.centerIn: parent
        spacing: 8
        Text { text: "Standalone fixture loaded" }
        Text { text: callResult }
    }

    Component.onCompleted: {
        logos.callModuleAsync("package_manager", "getInstalledPackages", [], function(payload) {
            let parsed = null
            try { parsed = JSON.parse(payload) } catch (e) {}
            const failed = !parsed || ("error" in parsed && !("result" in parsed))
            callResult = failed ? "Bridge call: failed " + payload : "Bridge call: ok"
        }, 20000)
    }
}
