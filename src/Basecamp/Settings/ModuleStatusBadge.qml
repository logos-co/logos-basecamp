import QtQuick

import Logos.Controls
import Logos.Theme

// Load-state badge for the inspector tables. `row` is a ModuleTableModel row —
// the normalised flags on it (isMainUi / hasMissingDeps / isLoaded) pick the
// colour, `statusText` supplies the label so the wording lives in one place.
LogosBadge {
    id: root

    property var row: null

    text: row ? row.statusText : ""
    color: !row                       ? Theme.palette.textTertiary
         : row.isMainUi               ? Theme.palette.info
         : row.hasMissingDeps         ? Theme.palette.warning
         : row.isLoaded               ? Theme.palette.success
                                      : Theme.palette.textTertiary
}
