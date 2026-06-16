import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

import Basecamp.Panels
import Basecamp.Views

Rectangle {
    id: root

    readonly property QtObject sections: QtObject {
        readonly property int workspace: 0
        readonly property int apps: 1
        readonly property int packageManager: 2
        readonly property int settings: 3
    }

    color: Theme.palette.background

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spacing.tiny
        anchors.topMargin: 0
        anchors.rightMargin: Theme.spacing.tiny
        anchors.bottomMargin: 2  // off-scale (no spacing token for 2)
        spacing: 0

        SidebarPanel {
            id: sidebar
            Layout.preferredWidth: 60
            Layout.minimumWidth: 60
            Layout.maximumWidth: 60
            Layout.fillHeight: true
            onLaunchUIModule: (name) => Qt.callLater(() => backend.onAppLauncherClicked(name))
            onUpdateLauncherIndex: (index) => backend.setCurrentActiveSectionIndex(index)
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacing.tiny
                anchors.topMargin: 9
                anchors.rightMargin: Theme.spacing.tiny
                anchors.bottomMargin: Theme.spacing.tiny
                spacing: 0

                StackLayout {
                    id: contentStack
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: {
                        const s = backend.currentActiveSectionIndex
                        if (s === root.sections.workspace) return 0
                        if (s === root.sections.packageManager) return 2
                        return 1
                    }

                    // idx 0 — Apps (was MdiView, C++ widget — placeholder
                    // until WorkspaceView.qml lands)
                    Rectangle {
                        color: Theme.palette.backgroundInset
                        Text {
                            anchors.centerIn: parent
                            text: "Workspace PlaceHolder"
                            color: Theme.palette.textSecondary
                            font.pixelSize: Theme.typography.primaryText
                        }
                    }

                    ContentViews { id: contentViews }

                    Item {
                        // Package Manager
                        Loader {
                            id: pmuiLoader
                            anchors.fill: parent
                            asynchronous: true
                            source: backend.packageManagerViewUrl
                            onStatusChanged: {
                                if (status === Loader.Error)
                                    console.warn("PMUI loader failed:", source)
                            }
                        }

                        LogosSpinner {
                            anchors.centerIn: parent
                            visible: pmuiLoader.status !== Loader.Ready
                            running: visible
                        }
                    }
                }
            }
        }
    }

    OverlayDialogs {
        id: overlay
        anchors.fill: parent
        z: 1000
    }
}
