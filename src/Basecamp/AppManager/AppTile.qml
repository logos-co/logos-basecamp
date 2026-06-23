import QtQuick
import QtQuick.Controls
import Logos.Theme
import Logos.Controls

// Shared app tile: dark muted background, package icon or monogram.
Control {
    id: root

    property string appName: ""
    property string monogramLabel: ""
    property string packageColor: ""
    property url iconSource: ""
    property real dimOpacity: 1.0

    property int tileSize: 40
    property int iconSize: 0
    property int radius: Theme.spacing.radiusMedium
    property bool borderOnHover: false
    // Parent delegate hover (Control.hovered is FINAL and only tracks this item).
    property bool borderEmphasized: false

    readonly property int effectiveIconSize:
        iconSize > 0 ? iconSize : Math.round(tileSize * 0.5)
    readonly property string monogram:
        ((monogramLabel || appName) || "?").substring(0, 2).toUpperCase()
    readonly property bool hasIcon: iconSource.toString().length > 0

    implicitWidth: tileSize
    implicitHeight: tileSize
    padding: 0
    opacity: dimOpacity
    Behavior on opacity { NumberAnimation { duration: 150 } }

    background: Rectangle {
        radius: root.radius
        color: AppColors.tileColor(root.packageColor, root.appName)
        border.width: 1
        border.color: root.borderOnHover && root.borderEmphasized
                      ? Theme.palette.border
                      : Theme.palette.borderSubtle
    }

    contentItem: Item {
        LogosIcon {
            anchors.centerIn: parent
            width: root.effectiveIconSize
            height: root.effectiveIconSize
            source: root.iconSource
            color: Theme.palette.text
            visible: root.hasIcon
        }

        LogosText {
            anchors.centerIn: parent
            text: root.monogram
            font.pixelSize: Math.round(root.tileSize * 0.375)
            font.weight: Theme.typography.weightBold
            color: Theme.palette.text
            visible: !root.hasIcon
        }
    }
}
