import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls

// Shared row shell for the Modules list — used by both UiModulesTab and
// CoreModulesView. Renders the alternating-stripe card + the horizontal
// RowLayout; callers supply the row contents as default children.
//
// Sizing contract:
//   * `rowHeight`   — preferred height for the row. Both tabs use 50 today.
//   * `rowIndex`    — for the alternating-stripe colour; pass the delegate's
//                     `index` property.
//
// Layout contract for children:
//   * Give the "name" text `Layout.fillWidth: true` + `elide: Text.ElideRight`
//     so long module names (e.g. `liblogos_blockchain_module`) elide instead
//     of bleeding into the next column.
//   * Give any other text/button columns explicit `Layout.preferredWidth` (or
//     implicit via button `implicitWidth`) — those are the "fixed" columns,
//     the name absorbs the remaining space.
//   * A trailing `Item { Layout.fillWidth: true }` spacer is NO LONGER needed
//     — the flex-sized name absorbs the slack. Putting two `fillWidth`
//     siblings in the same row actively fought layout and was the root of
//     the UI Modules overlap bug.
Rectangle {
    id: root

    property int rowIndex: 0
    property int rowHeight: 50

    // Default property: everything nested inside a `ModuleRow { ... }` instance
    // is forwarded into the inner RowLayout via `data` aliasing.
    default property alias rowContent: rowLayout.data

    Layout.fillWidth: true
    Layout.preferredHeight: rowHeight
    color: rowIndex % 2 === 0 ? "#363636" : "#2d2d2d"
    radius: 6

    RowLayout {
        id: rowLayout
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 10
    }
}
