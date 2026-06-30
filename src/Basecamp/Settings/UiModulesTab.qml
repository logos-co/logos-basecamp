import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls

Item {
    id: root

    ColumnLayout {
        anchors.fill: parent
        spacing: 20

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                LogosText {
                    text: "UI Modules"
                    font.pixelSize: 20
                    font.weight: Font.Bold
                    color: "#ffffff"
                }

                LogosText {
                    text: "Available UI plugins in the system"
                    color: "#a0a0a0"
                }
            }

            // Reload — matches the Core Modules tab. Pushes a full metadata
            // + dependency-info refresh; useful after installing a package
            // from disk when you want to see installType / deps update
            // without restarting Basecamp.
            Button {
                text: "Reload"
                onClicked: backend.refreshUiModules()

                contentItem: LogosText {
                    text: parent.text
                    font.pixelSize: 13
                    color: "#ffffff"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    implicitWidth: 100
                    implicitHeight: 32
                    color: parent.pressed ? "#3d3d3d" : "#4d4d4d"
                    radius: 4
                    border.color: "#5d5d5d"
                    border.width: 1
                }
            }
        }

        // Outer card — same shape as CoreModulesView so the two tabs
        // sit visually consistent. Hosting the ScrollView inside a
        // sized Rectangle (rather than a Layout cell) is what lets the
        // inner `ColumnLayout { width: parent.width }` size itself
        // correctly; the previous version used a Layout-cell ScrollView
        // and per-row bordered cards, which fought each other for width
        // and produced overlapping rows.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#2d2d2d"
            radius: 8
            border.color: "#3d3d3d"
            border.width: 1

            ScrollView {
                id: scroll
                anchors.fill: parent
                anchors.margins: 20
                clip: true

                // Inner ColumnLayout must key off the ScrollView's
                // `availableWidth` rather than `parent.width`. In Qt 6, a
                // ScrollView reparents its child into an internal Flickable
                // whose implicit contentItem can transiently resolve to 0
                // width while the Repeater is recreating delegates in
                // response to a model change (e.g., after package_manager_ui
                // loads and `uiModulesChanged` fires). That snaps
                // `Layout.fillWidth: true` children to width 0 and doesn't
                // always recover. `availableWidth` is the public, stable
                // property that already accounts for scrollbars and is safe
                // to bind to directly.
                ColumnLayout {
                    width: scroll.availableWidth
                    spacing: 8

                    Repeater {
                        model: backend.uiModulesProxy

                        // Row shell is shared with CoreModulesView so the two
                        // tabs stay visually consistent. Column layout rules:
                        //   * Name text takes `Layout.fillWidth` + elide — it
                        //     absorbs slack and shrinks gracefully on narrow
                        //     windows without shoving the buttons off-screen.
                        //   * Every other column has an explicit
                        //     `Layout.preferredWidth` (directly or via the
                        //     button's implicit background width).
                        //   * NO trailing fillWidth spacer. A previous version
                        //     had BOTH a fixed-width name AND a fillWidth
                        //     spacer in the same row, which fought layout and
                        //     collapsed everything onto the left edge.
                        delegate: ModuleRow {
                            required property int index
                            required property string name
                            required property string displayName
                            required property string iconPath
                            required property bool isMainUi
                            required property bool isLoaded
                            required property string installType

                            rowIndex: index
                            rowHeight: 50

                            // Icon tile — unique to the UI Modules tab.
                            Rectangle {
                                Layout.preferredWidth: 34
                                Layout.preferredHeight: 34
                                radius: 6
                                color: "#3d3d3d"

                                Image {
                                    anchors.centerIn: parent
                                    source: iconPath || ""
                                    sourceSize.width: 24
                                    sourceSize.height: 24
                                    visible: iconPath && iconPath.length > 0
                                }

                                LogosText {
                                    anchors.centerIn: parent
                                    text: name.substring(0, 2).toUpperCase()
                                    font.pixelSize: 12
                                    font.weight: Font.Bold
                                    color: "#808080"
                                    visible: !iconPath || iconPath.length === 0
                                }
                            }

                            // Name absorbs the remaining row width.
                            LogosText {
                                text: displayName || name
                                font.pixelSize: 16
                                color: "#e0e0e0"
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                Layout.minimumWidth: 80
                            }

                            // Status — fixed width so it aligns across rows
                            // regardless of which of the three states is
                            // showing ("(Main UI)" / "(Loaded)" / "(Not Loaded)").
                            LogosText {
                                text: isMainUi ? "(Main UI)" :
                                      (isLoaded ? "(Loaded)" : "(Not Loaded)")
                                color: isMainUi ? "#a0a0a0" :
                                       (isLoaded ? "#4CAF50" : "#F44336")
                                Layout.preferredWidth: 100
                            }

                            // Load / Unload toggle (hidden for the main UI
                            // module — that's us, can't unload ourselves).
                            Button {
                                text: isLoaded ? "Unload Plugin" : "Load Plugin"
                                visible: !isMainUi

                                contentItem: LogosText {
                                    text: parent.text
                                    font.pixelSize: 12
                                    color: "#ffffff"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                background: Rectangle {
                                    implicitWidth: 100
                                    implicitHeight: 30
                                    color: isLoaded ?
                                        (parent.pressed ? "#da190b" : "#F44336") :
                                        (parent.pressed ? "#3d8b40" : "#4b4b4b")
                                    radius: 4
                                }

                                onClicked: {
                                    if (isLoaded) {
                                        backend.unloadUiModule(name)
                                    } else {
                                        backend.loadUiModule(name)
                                    }
                                }
                            }

                            // Uninstall — only for user-installed modules.
                            Button {
                                text: "Uninstall"
                                visible: !isMainUi
                                         && installType === "user"

                                contentItem: LogosText {
                                    text: parent.text
                                    font.pixelSize: 12
                                    color: "#ffffff"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                background: Rectangle {
                                    implicitWidth: 90
                                    implicitHeight: 30
                                    color: parent.pressed ? "#6d6d6d" : "#8b8b8b"
                                    radius: 4
                                }

                                onClicked: backend.uninstallUiModule(name)
                            }
                        }
                    }

                    // Empty state
                    LogosText {
                        text: "No UI plugins found in the plugins directory."
                        color: "#606060"
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 40
                        visible: backend.uiModulesProxy.count === 0
                    }
                }
            }
        }
    }
}
