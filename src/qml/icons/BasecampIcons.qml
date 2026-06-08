pragma Singleton
import QtQuick

import Logos.Icons

QtObject {
    readonly property url logo:       Qt.resolvedUrl("basecamp.svg")
    readonly property url tents:      Qt.resolvedUrl("tent.png")
    readonly property url dashboard:  Qt.resolvedUrl("dashboard.svg")
    readonly property url modules:    Qt.resolvedUrl("module.svg")
    readonly property url settings:   Qt.resolvedUrl("settings.svg")
    readonly property url workspace:  Qt.resolvedUrl("workspace.svg")
}
