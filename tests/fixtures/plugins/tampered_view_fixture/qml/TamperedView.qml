import QtQuick

// A perfectly good view. nix/integration-test.nix installs it and then edits
// this file, so Basecamp's launch-time integrity check must refuse it before
// any QML is read. Do not "break" this file: the test is that a VALID view
// never loads once its installed copy no longer matches its hashes.
Rectangle {
    color: "#101318"
    Text {
        anchors.centerIn: parent
        color: "white"
        text: "TAMPERED VIEW FIXTURE: this should never be on screen"
    }
}
