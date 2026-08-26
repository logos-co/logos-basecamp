import QtQuick
import QtQuick.Controls

import Logos.Theme
import Logos.Controls
import Basecamp.AppManager

// AbstractButton so the whole 50px row is one mouse, keyboard and
// accessibility control — matching the grid and list delegates, and avoiding
// two focus stops per app from a nested interactive tile.
//
// `iconSource` is a custom property rather than AbstractButton's
// `icon.source` group property, whose sub-property assignments do not
// reliably re-fire bindings in Qt 6.
AbstractButton {
    id: root

    property url iconSource: ""
    property bool fullBleedIcon: false

    property bool loaded: false
    property bool loading: false
    // True iff the backend reports this plugin has core dependencies that
    // won't let it load. When set, a marker renders top-right — clicking
    // still emits `clicked`, and the backend decides whether to load or show
    // the popup.
    property bool hasMissingDeps: false
    // WHICH kind of dependency problem:
    // "" | "absent" | "mismatch" | "signer" | "mixed".
    // A dependency that is installed but REJECTED — wrong version, or a
    // package under the right name from the wrong publisher — is a different
    // problem with a different remedy, so it gets its own marker rather than
    // borrowing the "not installed" cross. `hasMissingDeps` still owns
    // visibility, so a payload without this field renders exactly as before.
    property string depBlockKind: ""

    // Everything needed is ON DISK and this app rejects it. "mixed" is
    // excluded deliberately: something IS absent there, and absence is the
    // fact the user has to act on first, so it keeps the cross.
    readonly property bool _presentButRejected: root.depBlockKind === "mismatch"
                                             || root.depBlockKind === "signer"
    property string appName: ""

    // Test hook: whether this app is the front-most (active) one.
    readonly property bool active: checked

    implicitHeight: 50
    hoverEnabled: true

    QtObject {
        id: d

        readonly property int tileSize: 38
    }

    signal tooltipRequested(string text, real y)

    onHoveredChanged: {
        if (hovered && text) {
            var pos = root.mapToItem(null, root.width, root.height / 2)
            root.tooltipRequested(text, pos.y)
        }
    }

    background: Item {
        Rectangle {
            anchors.left: parent.left
            width: 3
            height: parent.height
            color: root.checked ? Theme.palette.accentOrange : "transparent"
        }
    }

    contentItem: Item {
        LogosTile {
            id: tile
            anchors.centerIn: parent
            label: root.appName
            source: root.iconSource
            fallbackColor: Theme.palette.surfaceRaised
            highlighted: root.checked
            insetArtwork: !root.fullBleedIcon
            // Presentational — the row owns activation.
            interactive: false
            tileSize: d.tileSize
            dimOpacity: root.loaded ? 1.0 : 0.55
            visible: !root.loading
        }

        Item {
            id: spinner
            anchors.centerIn: parent
            width: 20
            height: 20
            visible: root.loading

            RotationAnimator on rotation {
                running: spinner.visible
                from: 0
                to: 360
                duration: 900
                loops: Animation.Infinite
            }

            Repeater {
                model: 8
                Rectangle {
                    required property int index
                    width: 3
                    height: 3
                    radius: 1.5
                    color: Theme.palette.textSecondary
                    opacity: (index + 1) / 8
                    x: spinner.width / 2 + 7 * Math.cos(index * Math.PI / 4) - width / 2
                    y: spinner.height / 2 + 7 * Math.sin(index * Math.PI / 4) - height / 2
                }
            }
        }

        Rectangle {
            id: missingDepsMarker
            // Three names, so a UI test can tell the three states apart —
            // and the amber marker means two different things, which a
            // screenshot cannot distinguish.
            objectName: root.depBlockKind === "signer" ? "sidebar.marker.signerConflict"
                      : root._presentButRejected       ? "sidebar.marker.versionConflict"
                                                       : "sidebar.marker.missingDeps"
            visible: root.hasMissingDeps && !root.loading
            width: 14
            height: 14
            radius: 7
            // Red cross = something is absent. Amber "!" = everything needed
            // is present, but this app rejects it — wrong version, or the
            // wrong publisher's package under the right name. States the user
            // resolves differently, so they must not look identical.
            // "mixed" keeps the cross: something IS absent.
            color: root._presentButRejected ? "#e8a33d" : "#d32f2f"
            anchors.right: tile.right
            anchors.top: tile.top
            anchors.rightMargin: -2
            anchors.topMargin: -2

            // Cross — absent (or mixed).
            Rectangle {
                visible: !root._presentButRejected
                width: 8
                height: 1.5
                color: "white"
                anchors.centerIn: parent
                rotation: 45
            }
            Rectangle {
                visible: !root._presentButRejected
                width: 8
                height: 1.5
                color: "white"
                anchors.centerIn: parent
                rotation: -45
            }

            // Exclamation — version conflict.
            Rectangle {
                visible: root._presentButRejected
                width: 1.5
                height: 5
                radius: 0.75
                color: "white"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 3
            }
            Rectangle {
                visible: root._presentButRejected
                width: 1.5
                height: 1.5
                radius: 0.75
                color: "white"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 3
            }
        }
    }
}
