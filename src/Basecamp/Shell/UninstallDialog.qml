import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Controls
import Logos.Icons
import Logos.Theme

// The one uninstall confirmation. Plan payload shape:
//   { kind, multi, batch, removable[], kept[], dependents[], loading?, targetName? }
// See PackageCoordinator::buildPlanPayload for row fields.
LogosWarningDialog {
    id: root

    property var plan: ({})

    signal confirmed(var plan)
    signal cancelled(var plan)
    signal cancelledWhileLoading(var plan)

    function openWithPlan(plan_) {
        root.plan = plan_ || ({});
        open();
    }

    QtObject {
        id: d

        readonly property var removable:  (root.plan && root.plan.removable)  ? root.plan.removable  : []
        readonly property var kept:       (root.plan && root.plan.kept)       ? root.plan.kept       : []
        readonly property var dependents: (root.plan && root.plan.dependents) ? root.plan.dependents : []
        readonly property var batch:      (root.plan && root.plan.batch)      ? root.plan.batch      : []
        readonly property string kind:    (root.plan && root.plan.kind) ? root.plan.kind : "packages"
        // Host-attested. Empty when the user acted through the shell's own UI.
        readonly property string requester:
            (root.plan && root.plan.requester) ? root.plan.requester : ""
        readonly property bool requesterBundled:
            !!(root.plan && root.plan.requesterBundled)
        // Resolved by the shell; falls back to the module id when the catalog
        // knows no friendlier label.
        readonly property string requesterLabel:
            (root.plan && root.plan.requesterDisplayName)
                ? root.plan.requesterDisplayName : d.requester
        readonly property bool loading:   root.plan && root.plan.loading === true
        readonly property string loadingLabel:
            root.plan && root.plan.targetName ? root.plan.targetName : ""

        readonly property var primaryRow: {
            for (var i = 0; i < d.removable.length; ++i)
                if (d.removable[i].isTarget) return d.removable[i];
            return d.removable.length > 0 ? d.removable[0] : null;
        }
        // Fall back to loading targetName while the real plan is en route.
        readonly property string primaryLabel: {
            if (d.loading && d.loadingLabel) return d.loadingLabel;
            return d.primaryRow ? (d.primaryRow.displayName || d.primaryRow.name) : "";
        }

        readonly property int broughtInCount: {
            var n = 0;
            for (var i = 0; i < d.removable.length; ++i)
                if (!d.removable[i].isTarget) ++n;
            return n;
        }

        function keptReasonText(row) {
            if (!row) return "";
            if (row.reason === "embedded")  return qsTr("built in");
            if (row.reason === "protected") return qsTr("required by the app");
            if (row.reason === "requiredBy") {
                const names = row.requiredBy || [];
                if (names.length === 0) return qsTr("still required");
                // Show 3 names, then "+N more" so the line doesn't wrap.
                const shown = names.slice(0, 3).join(", ");
                return names.length > 3
                    ? qsTr("still required by %1 +%2 more").arg(shown).arg(names.length - 3)
                    : qsTr("still required by %1").arg(shown);
            }
            return qsTr("no longer needed by anything");
        }

        function removableRoleText(row) {
            if (!row) return "";
            if (!row.isTarget) return qsTr("dependency");
            return d.kind === "app" ? qsTr("app") : qsTr("package");
        }
    }

    width: 560
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape
    accentColor: Theme.palette.error
    iconSource: LogosIcons.warning
    title: d.batch.length > 1 && d.kind !== "app"
           ? qsTr("Uninstall %1 packages?").arg(d.batch.length)
           : qsTr("Uninstall \"%1\"?").arg(d.primaryLabel)

    rightActions: [
        LogosButton {
            objectName: "uninstallDialog.cancelButton"
            text: qsTr("Cancel")
            onClicked: root.reject()
        },
        LogosButton {
            objectName: "uninstallDialog.confirmButton"
            text: d.batch.length > 1
                  ? qsTr("Uninstall %1").arg(d.batch.length)
                  : qsTr("Uninstall")
            variant: LogosButton.Variant.Primary
            enabled: !d.loading
            onClicked: root.accept()
        }
    ]

    onAccepted: root.confirmed(root.plan)
    onRejected: {
        if (d.loading) root.cancelledWhileLoading(root.plan);
        else           root.cancelled(root.plan);
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacing.medium

        // ─── Loading state ───
        RowLayout {
            objectName: "uninstallDialog.loadingRow"
            Layout.fillWidth: true
            spacing: Theme.spacing.medium
            visible: d.loading

            LogosSpinner {
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                running: d.loading
            }

            LogosText {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.palette.textSecondary
                text: qsTr("Preparing removal plan…")
            }
        }

        // ─── Who asked ── only when the request crossed an app boundary.
        // The package name, never the self-declared display name.
        LogosText {
            objectName: "uninstallDialog.requester"
            Layout.fillWidth: true
            visible: d.requester.length > 0
            wrapMode: Text.Wrap
            color: Theme.palette.textSecondary
            font.pixelSize: Theme.typography.secondaryText
            // See ConfirmationDialog for why bundled shows the label alone and
            // everything else shows both.
            text: d.requesterBundled
                  ? qsTr("Requested by %1, which ships with Logos.").arg(d.requesterLabel)
                  : (d.requesterLabel === d.requester
                     ? qsTr("Requested by %1 — not part of Logos, and unsigned, so the shell cannot confirm who published it.")
                         .arg(d.requesterLabel)
                     : qsTr("Requested by %1 (%2) — not part of Logos, and unsigned, so the shell cannot confirm who published it.")
                         .arg(d.requesterLabel).arg(d.requester))
        }

        // ─── Body ───
        LogosText {
            objectName: "uninstallDialog.body"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.palette.textSecondary
            visible: !d.loading
            text: {
                if (d.broughtInCount > 0) {
                    return qsTr("%1 and %2 package(s) it brought in will be removed.")
                             .arg(d.primaryLabel).arg(d.broughtInCount);
                }
                if (d.batch.length > 1) {
                    return qsTr("%1 packages will be removed from disk.")
                             .arg(d.batch.length);
                }
                return qsTr("This will remove the package files from disk.");
            }
        }

        // ─── Dependent warning ── shown only when something actually breaks.
        // Not a veto: the user asked for the removal.
        ColumnLayout {
            objectName: "uninstallDialog.dependentsSection"
            Layout.fillWidth: true
            spacing: Theme.spacing.small
            visible: d.dependents.length > 0

            LogosText {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.palette.warning
                font.pixelSize: Theme.typography.secondaryText
                font.weight: Theme.typography.weightMedium
                text: qsTr("⚠ %1 installed module(s) depend on %2 and will stop working:")
                        .arg(d.dependents.length).arg(d.primaryLabel)
            }

            PlanList {
                Layout.fillWidth: true
                model: d.dependents
                labelFor: function(row) {
                    return (row.displayName || row.name)
                         + (row.isLoaded ? qsTr(" (running — unloaded now)") : "");
                }
            }
        }

        // ─── Will be removed ───
        ColumnLayout {
            objectName: "uninstallDialog.removableSection"
            Layout.fillWidth: true
            spacing: Theme.spacing.small
            visible: d.removable.length > 0

            LogosText {
                Layout.fillWidth: true
                text: qsTr("Will be removed")
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
                font.weight: Theme.typography.weightBold
            }

            PlanList {
                Layout.fillWidth: true
                model: d.removable
                labelFor: function(row) { return row.displayName || row.name; }
                // No size column — real on-disk size isn't tracked.
                detailFor: function(row) {
                    const ver = row.version ? "v" + row.version : "";
                    const role = d.removableRoleText(row);
                    return ver ? (ver + "   " + role) : role;
                }
            }
        }

        // ─── Kept ───
        ColumnLayout {
            objectName: "uninstallDialog.keptSection"
            Layout.fillWidth: true
            spacing: Theme.spacing.small
            visible: d.kept.length > 0

            LogosText {
                Layout.fillWidth: true
                text: qsTr("Kept")
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
                font.weight: Theme.typography.weightBold
            }

            PlanList {
                Layout.fillWidth: true
                model: d.kept
                labelFor: function(row) { return row.displayName || row.name; }
                detailFor: function(row) { return d.keptReasonText(row); }
            }
        }
    }

    // Bulleted "name … detail" list, scrolls past ~120px.
    component PlanList: Rectangle {
        property var model: []
        property var labelFor: function(row) { return row.name || ""; }
        property var detailFor: null

        color: Theme.palette.background
        radius: Theme.spacing.radiusSmall
        border.color: Theme.palette.borderSubtle
        border.width: 1
        implicitHeight: Math.min(120, Math.max(30, listView.contentHeight + 16))

        LogosListView {
            id: listView
            anchors.fill: parent
            anchors.margins: Theme.spacing.small
            model: parent.model
            interactive: contentHeight > height

            delegate: RowLayout {
                required property var modelData
                width: ListView.view ? ListView.view.width : 0
                spacing: Theme.spacing.small

                LogosText {
                    Layout.fillWidth: true
                    text: "• " + listView.parent.labelFor(modelData)
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.secondaryText
                    elide: Text.ElideRight
                }

                LogosText {
                    visible: text.length > 0
                    text: listView.parent.detailFor
                          ? listView.parent.detailFor(modelData)
                          : ""
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
            }
        }
    }
}
