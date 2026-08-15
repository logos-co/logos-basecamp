#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantList>

// One row per loaded/known module, normalised so QML can bind by named role
// (`model.label`, `model.isLoaded`, `model.cpu`, …). Rows come from
// MainUIBackend::buildUiModulesSnapshot() /
// MainUIBackend::buildCoreModulesSnapshot() (composed over UIPluginManager +
// CoreModuleManager + PackageCoordinator) as QVariantList maps — this model
// wraps that data in a real Qt model so the inspectors don't need a
// QML-side adapter (see the deleted ModuleTableModel.qml).
//
// Two consumers share the same schema: the UI-plugin inspector uses the
// version/description/iconPath columns; the core-module inspector uses
// cpu/memory. Roles unused by either side simply render as empty.
//
// `replaceRows` patches in place when the row identities are unchanged, so the
// 2-second core-stats poll doesn't reset the model (which would drop the
// selection + flicker every delegate).
class ModuleInstanceModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        LabelRole,             // displayName if set, else name
        DescriptionRole,
        CategoryRole,
        TypeRole,              // "ui_qml" | "core" | ""
        VersionRole,
        IconPathRole,
        InstallTypeRole,       // "user" | "embedded" | ""
        IsLoadedRole,
        IsMainUiRole,
        HasMissingDepsRole,
        StatusTextRole,        // derived: Main UI / Missing deps / Loaded / Not loaded
        CpuRole,               // core modules only; 0 when unknown
        MemoryRole,            // core modules only; 0 when unknown
    };
    Q_ENUM(Roles)

    explicit ModuleInstanceModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replace the model with normalised rows derived from `installedModules`
    // (the QVariantList shape produced by MainUIBackend's snapshot builders).
    // Patches in place when the incoming rows share names 1:1 with existing
    // rows in the same order — that's the common case on the stats poll.
    void replaceRows(const QVariantList& installedModules);

private:
    struct Row {
        QString name;
        QString label;
        QString description;
        QString category;
        QString type;
        QString version;
        QString iconPath;
        QString installType;
        bool    isLoaded       = false;
        bool    isMainUi       = false;
        bool    hasMissingDeps = false;
        double  cpu            = 0.0;
        double  memory         = 0.0;

        QString statusText() const;
    };

    static Row toRow(const QVariantMap& m);
    // Returns the diff mask for a single row so patchRow can emit dataChanged
    // with only the roles that actually moved. Empty vector = identical.
    static QList<int> diffRoles(const Row& a, const Row& b);

    QList<Row> m_rows;
};
