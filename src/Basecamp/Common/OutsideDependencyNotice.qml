import QtQuick
import QtQuick.Layouts

import Logos.Controls
import Logos.Theme

ColumnLayout {
    id: root

    // [{name, version, installed, repository}]
    property var dependencies: []

    visible: root.dependencies.length > 0
    spacing: Theme.spacing.small
    Layout.fillHeight: false

    QtObject {
        id: d
        function fill(template, values) {
            return template.replace(/%([1-9])/g, (match, n) => n <= values.length ? values[n - 1] : match)
        }
    }

    LogosNotice {
        objectName: "outsideDependencies"
        Layout.fillWidth: true
        shown: root.dependencies.length > 0
        severity: LogosNotice.Warning
        message: root.dependencies.map(dep => dep.installed
            ? d.fill(qsTr("This needs “%1”, which its repository doesn't provide. It will use the copy already installed (%2)."),
                     [dep.name, dep.version])
            : d.fill(qsTr("This needs “%1”, which its repository doesn't provide. It will be installed from %2."),
                     [dep.name, dep.repository])).join("\n")
    }
}
