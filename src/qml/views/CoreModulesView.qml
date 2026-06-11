import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import panels

Item {
    id: root
    objectName: "coreModulesView"

    property string selectedPlugin: ""
    property bool showingInterface: false

    // Open a specific module's Interface screen (methods + events) by name.
    // Equivalent to clicking that module's "Interface" button — exposed for UI
    // automation/tests, which can't disambiguate the per-row buttons by their
    // identical label.
    function openInterface(name) {
        selectedPlugin = name;
        showingInterface = true;
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: root.showingInterface ? 1 : 0

        // Plugin list view
        ColumnLayout {
            spacing: 20

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    LogosText {
                        text: "Core Modules"
                        font.pixelSize: 20
                        font.weight: Font.Bold
                        color: "#ffffff"
                    }

                    LogosText {
                        text: "All available plugins in the system"
                        color: "#a0a0a0"
                    }
                }

                Button {
                    text: "Reload"
                    onClicked: backend.refreshCoreModules()

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

                    // See UiModulesTab.qml for the rationale — `parent.width`
                    // inside a Qt 6 ScrollView resolves against the internal
                    // Flickable's contentItem, which can transiently collapse
                    // to 0 during Repeater reflow. `availableWidth` is stable.
                    ColumnLayout {
                        width: scroll.availableWidth
                        spacing: 8

                        Repeater {
                            model: backend.coreModules

                            // Row shell is shared with UiModulesTab. Column
                            // rules: name absorbs slack via `Layout.fillWidth`
                            // + elide so long names like
                            // `liblogos_blockchain_module` don't bleed into
                            // the status column. Every other column uses an
                            // explicit `Layout.preferredWidth`.
                            ModuleRow {
                                rowIndex: index
                                rowHeight: 50

                                // Plugin name — flex-sized.
                                LogosText {
                                    text: modelData.name
                                    font.pixelSize: 16
                                    color: "#e0e0e0"
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 80
                                }

                                // Status — fixed width so CPU/Mem columns
                                // line up the same whether the row says
                                // "(Loaded)" or "(Not Loaded)".
                                LogosText {
                                    text: modelData.isLoaded ? "(Loaded)" : "(Not Loaded)"
                                    color: modelData.isLoaded ? "#4CAF50" : "#F44336"
                                    Layout.preferredWidth: 100
                                }

                                // CPU (blank for not-loaded, but the column
                                // stays so the row shape is stable).
                                LogosText {
                                    text: modelData.isLoaded ? "CPU: " + modelData.cpu + "%" : ""
                                    color: "#64B5F6"
                                    Layout.preferredWidth: 80
                                }

                                // Memory
                                LogosText {
                                    text: modelData.isLoaded ? "Mem: " + modelData.memory + " MB" : ""
                                    color: "#81C784"
                                    Layout.preferredWidth: 100
                                }

                                // Load/Unload button
                                Button {
                                    text: modelData.isLoaded ? "Unload Plugin" : "Load Plugin"

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
                                        color: modelData.isLoaded ?
                                            (parent.pressed ? "#da190b" : "#F44336") :
                                            (parent.pressed ? "#3d8b40" : "#4b4b4b")
                                        radius: 4
                                    }

                                    onClicked: {
                                        if (modelData.isLoaded) {
                                            backend.unloadCoreModule(modelData.name)
                                        } else {
                                            backend.loadCoreModule(modelData.name)
                                        }
                                    }
                                }

                                // Interface button (methods + events; only for loaded)
                                Button {
                                    text: "Interface"
                                    visible: modelData.isLoaded

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
                                        color: parent.pressed ? "#3d3d3d" : "#4b4b4b"
                                        radius: 4
                                    }

                                    onClicked: {
                                        root.selectedPlugin = modelData.name
                                        root.showingInterface = true
                                    }
                                }

                                // Uninstall button — only shown for
                                // user-installed core modules. Embedded
                                // modules (package_manager, core_manager,
                                // capability_module, etc. in release
                                // builds) are structurally protected.
                                // Backend also refuses non-user uninstalls
                                // so an accidental rogue call is a no-op.
                                Button {
                                    text: "Uninstall"
                                    visible: modelData.installType === "user"

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

                                    onClicked: backend.uninstallCoreModule(modelData.name)
                                }
                            }
                        }

                        // Empty state
                        LogosText {
                            text: "No core modules available."
                            color: "#606060"
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 40
                            visible: backend.coreModules.length === 0
                        }
                    }
                }
            }
        }

        // Interface view (methods + events)
        PluginInterfaceView {
            pluginName: root.selectedPlugin
            onBackClicked: root.showingInterface = false
        }
    }
}



