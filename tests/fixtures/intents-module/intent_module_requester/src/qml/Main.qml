import QtQuick

// "Ask with click" asks on the click. "Ask later" asks 3 s after it, when
// Basecamp no longer counts the click. tests/intents-module-tests.mjs asserts
// the texts.
Rectangle {
    id: root
    objectName: "requester.root"
    color: "white"

    readonly property var backend: logos.module("intent_module_requester")
    property bool ready: false
    property int queued: 0   // "Ask later" clicks not asked yet

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "intent_module_requester") root.ready = isReady && root.backend !== null
        }
    }
    Component.onCompleted: root.ready = root.backend !== null && logos.isViewModuleReady("intent_module_requester")

    Timer {
        id: later
        interval: 3000
        onTriggered: {
            root.queued -= 1
            root.backend.ask("test.module_echo", "late", false)
            if (root.queued > 0) later.start()
        }
    }

    Column {
        anchors.centerIn: parent
        width: 420
        spacing: 8
        Text { text: root.ready ? "Backend: connected" : "Backend: connecting" }
        Text { objectName: "requester.status"; text: "Status: " + (root.ready ? root.backend.status : "") }
        Text {
            objectName: "requester.waiting"
            text: "Waiting for an answer: " + (root.ready ? root.backend.waiting : 0)
        }
        Text {
            objectName: "requester.direct"
            text: "Direct: " + (root.ready ? root.backend.direct : "")
        }
        Row {
            spacing: 8
            Rectangle {
                objectName: "requester.ask"
                width: 130; height: 36
                color: "#ddd"
                Text { anchors.centerIn: parent; text: "Ask with click" }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.backend.ask("test.module_echo", "hello", true)
                }
            }
            Rectangle {
                objectName: "requester.askNoClick"
                width: 130; height: 36
                color: "#ddd"
                Text { anchors.centerIn: parent; text: "Ask later" }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        root.queued += 1
                        if (!later.running) later.start()
                    }
                }
            }
            Rectangle {
                objectName: "requester.cancelWaiting"
                width: 130; height: 36
                color: "#ddd"
                Text { anchors.centerIn: parent; text: "Cancel waiting" }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.backend.cancelWaiting()
                }
            }
        }
        // Basecamp's own intents, through the module: one it provides to anyone,
        // and one only the Package Manager may ask for. And an app that provides.
        Row {
            spacing: 8
            Rectangle {
                objectName: "requester.askApp"
                width: 130; height: 36
                color: "#ddd"
                Text { anchors.centerIn: parent; text: "Ask the app" }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.backend.ask("test.app_echo", "hi app", true)
                }
            }
            Rectangle {
                objectName: "requester.openSettings"
                width: 130; height: 36
                color: "#ddd"
                Text { anchors.centerIn: parent; text: "Open Settings" }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.backend.ask("basecamp.settings.open", "", true)
                }
            }
            Rectangle {
                objectName: "requester.confirmInstall"
                width: 130; height: 36
                color: "#ddd"
                Text { anchors.centerIn: parent; text: "Ask to install" }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.backend.ask("basecamp.packages.confirm_install", "", true)
                }
            }
        }
        Text {
            objectName: "requester.history"
            text: root.ready ? root.backend.history : ""
            font.pixelSize: 12
            color: "#444"
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            font.pixelSize: 11
            color: "#777"
            text: "A request with no click behind it waits in the pending list instead of "
                  + "popping up. The list holds 4 requests per app: the next one is refused "
                  + "(unavailable) until one is cancelled, or 30 minutes pass."
        }
    }
}
