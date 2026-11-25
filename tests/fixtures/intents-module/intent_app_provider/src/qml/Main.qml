import QtQuick

// Shows what intents_module delivered to the backend; Approve or Decline is the
// person's answer. tests/intents-module-tests.mjs asserts the texts.
Rectangle {
    id: root
    objectName: "provider.root"
    color: "white"

    readonly property var backend: logos.module("intent_app_provider")
    property bool ready: false

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "intent_app_provider") root.ready = isReady && root.backend !== null
        }
    }
    Component.onCompleted: root.ready = root.backend !== null && logos.isViewModuleReady("intent_app_provider")

    Column {
        anchors.centerIn: parent
        spacing: 8
        Text {
            objectName: "provider.request"
            text: "Request: " + (root.ready && root.backend.request !== "" ? root.backend.request : "none")
        }
        Row {
            spacing: 8
            Rectangle {
                objectName: "provider.approve"
                width: 120; height: 36
                color: "#ddd"
                Text { anchors.centerIn: parent; text: "Approve" }
                MouseArea { anchors.fill: parent; onClicked: root.backend.approve() }
            }
            Rectangle {
                objectName: "provider.decline"
                width: 120; height: 36
                color: "#ddd"
                Text { anchors.centerIn: parent; text: "Decline" }
                MouseArea { anchors.fill: parent; onClicked: root.backend.decline() }
            }
        }
    }
}
