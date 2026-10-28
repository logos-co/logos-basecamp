import QtQuick

// A QML-only app with its own identity. tests/standalone-module-backend-tests.mjs
// calls attack() once module_backend_fixture's backend is running; every
// attempt must fail, while the control call to an unrestricted module must
// succeed (otherwise the refusals prove nothing).
Rectangle {
    id: root
    objectName: "intruder.root"
    color: "white"

    readonly property string target: "module_backend_fixture_backend"
    property string endpoint: "pending"
    property string action: "pending"
    property string replica: "pending"
    property string control: "pending"

    function outcome(payload) {
        let parsed = null
        try { parsed = JSON.parse(payload) } catch (e) {}
        // A successful call can answer any JSON value; only an object can carry an error.
        const failed = parsed === null || (typeof parsed === "object" && "error" in parsed && !("result" in parsed))
        const reached = !failed
        return (reached ? "REACHED " : "refused ") + String(payload).slice(0, 120)
    }

    function attack() {
        logos.callModuleAsync(target, "view_endpoint", [],
                              function(p) { root.endpoint = outcome(p) }, 10000)
        logos.callModuleAsync(target, "add", [2, 3],
                              function(p) { root.action = outcome(p) }, 10000)
        root.replica = logos.module("module_backend_fixture") === null ? "refused" : "REACHED"
        logos.callModuleAsync("package_manager", "getInstalledPackages", [],
                              function(p) { root.control = outcome(p) }, 10000)
    }

    Column {
        anchors.centerIn: parent
        spacing: 8
        Text { text: "Intruder view_endpoint: " + root.endpoint }
        Text { text: "Intruder call: " + root.action }
        Text { text: "Intruder replica: " + root.replica }
        Text { text: "Intruder control: " + root.control }
    }
}
