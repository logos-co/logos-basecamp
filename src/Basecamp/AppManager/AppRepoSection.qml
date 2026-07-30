import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

ColumnLayout {
    id: root

    property string title: ""
    property int count: 0
    property var modulesSource: null
    property string viewMode: "grid"

    signal appClicked(string name, string repositoryUrl)
    signal appManageRequested(string name, string repositoryUrl)

    Layout.fillWidth: true
    spacing: Theme.spacing.medium

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spacing.small

        LogosText {
            text: root.title
            font.pixelSize: Theme.typography.subtitleText
            font.weight: Theme.typography.weightMedium
            color: Theme.palette.textSecondary
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        LogosText {
            text: "(" + root.count + ")"
            font.pixelSize: Theme.typography.secondaryText
            color: Theme.palette.textTertiary
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Theme.palette.borderSubtle
        opacity: 0.5
    }

    AppGrid {
        Layout.fillWidth: true
        Layout.preferredHeight: implicitHeight
        modulesSource: root.modulesSource
        viewMode: root.viewMode
        onAppClicked: function(name, repositoryUrl) {
            root.appClicked(name, repositoryUrl || "")
        }
        onAppManageRequested: function(name, repositoryUrl) {
            root.appManageRequested(name, repositoryUrl || "")
        }
    }
}
