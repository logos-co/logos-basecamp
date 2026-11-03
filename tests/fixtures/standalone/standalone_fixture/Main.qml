import QtQuick

// Renders, then calls two core modules it depends on through the bridge:
// modules_state answers, and package_manager, which only Basecamp and the
// Package Manager may call, refuses. tests/standalone-tests.mjs asserts all
// three texts.
Rectangle {
    color: "white"

    property string callResult: "Bridge call: pending"
    property string packageManagerResult: "Package manager: pending"

    function failed(payload) {
        let parsed = null
        try { parsed = JSON.parse(payload) } catch (e) {}
        return parsed === null || (typeof parsed === "object" && "error" in parsed && !("result" in parsed))
    }

    Column {
        anchors.centerIn: parent
        spacing: 8
        Text { text: "Standalone fixture loaded" }
        Text { text: callResult }
        Text { text: packageManagerResult }
    }

    Component.onCompleted: {
        logos.callModuleAsync("modules_state", "list_modules", [], function(payload) {
            callResult = failed(payload) ? "Bridge call: failed " + payload : "Bridge call: ok"
        }, 20000)
        logos.callModuleAsync("package_manager", "getInstalledPackages", [], function(payload) {
            packageManagerResult = failed(payload) ? "Package manager: refused"
                                                   : "Package manager: REACHED " + payload
        }, 20000)
    }
}
