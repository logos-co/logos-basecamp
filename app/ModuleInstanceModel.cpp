#include "ModuleInstanceModel.h"

#include <QObject>
#include <QVariantMap>

ModuleInstanceModel::ModuleInstanceModel(QObject* parent)
    : QAbstractListModel(parent)
{}

int ModuleInstanceModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QString ModuleInstanceModel::Row::statusText() const
{
    if (isMainUi)       return QObject::tr("Main UI");
    if (hasMissingDeps) return QObject::tr("Missing deps");
    if (isLoaded)       return QObject::tr("Loaded");
    return QObject::tr("Not loaded");
}

QVariant ModuleInstanceModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row& r = m_rows[index.row()];
    switch (role) {
    case NameRole:            return r.name;
    case LabelRole:           return r.label.isEmpty() ? r.name : r.label;
    case DescriptionRole:     return r.description;
    case CategoryRole:        return r.category;
    case TypeRole:            return r.type;
    case VersionRole:         return r.version;
    case IconPathRole:        return r.iconPath;
    case InstallTypeRole:     return r.installType;
    case IsLoadedRole:        return r.isLoaded;
    case IsMainUiRole:        return r.isMainUi;
    case HasMissingDepsRole:  return r.hasMissingDeps;
    case StatusTextRole:      return r.statusText();
    case CpuRole:             return r.cpu;
    case MemoryRole:          return r.memory;
    }
    return {};
}

QHash<int, QByteArray> ModuleInstanceModel::roleNames() const
{
    return {
        {NameRole,            "name"},
        {LabelRole,           "label"},
        {DescriptionRole,     "description"},
        {CategoryRole,        "category"},
        {TypeRole,            "type"},
        {VersionRole,         "version"},
        {IconPathRole,        "iconPath"},
        {InstallTypeRole,     "installType"},
        {IsLoadedRole,        "isLoaded"},
        {IsMainUiRole,        "isMainUi"},
        {HasMissingDepsRole,  "hasMissingDeps"},
        {StatusTextRole,      "statusText"},
        {CpuRole,             "cpu"},
        {MemoryRole,          "memory"},
    };
}

ModuleInstanceModel::Row ModuleInstanceModel::toRow(const QVariantMap& m)
{
    Row r;
    r.name           = m.value(QStringLiteral("name")).toString();
    const QString displayName = m.value(QStringLiteral("displayName")).toString();
    r.label          = displayName.isEmpty() ? r.name : displayName;
    r.description    = m.value(QStringLiteral("description")).toString();
    r.category       = m.value(QStringLiteral("category")).toString();
    r.type           = m.value(QStringLiteral("type")).toString();
    r.version        = m.value(QStringLiteral("version")).toString();
    r.iconPath       = m.value(QStringLiteral("iconPath")).toString();
    r.installType    = m.value(QStringLiteral("installType")).toString();
    r.isLoaded       = m.value(QStringLiteral("isLoaded")).toBool();
    r.isMainUi       = m.value(QStringLiteral("isMainUi")).toBool();
    r.hasMissingDeps = m.value(QStringLiteral("hasMissingDeps")).toBool();
    // Stats arrive as either doubles or strings depending on what the module
    // reports — coerce both to double so the sort proxy always compares
    // numerically.
    bool ok = false;
    const double cpu = m.value(QStringLiteral("cpu")).toDouble(&ok);
    r.cpu = ok ? cpu : 0.0;
    ok = false;
    const double mem = m.value(QStringLiteral("memory")).toDouble(&ok);
    r.memory = ok ? mem : 0.0;
    return r;
}

QList<int> ModuleInstanceModel::diffRoles(const Row& a, const Row& b)
{
    QList<int> roles;
    if (a.label          != b.label)          roles.append(LabelRole);
    if (a.description    != b.description)    roles.append(DescriptionRole);
    if (a.category       != b.category)       roles.append(CategoryRole);
    if (a.type           != b.type)           roles.append(TypeRole);
    if (a.version        != b.version)        roles.append(VersionRole);
    if (a.iconPath       != b.iconPath)       roles.append(IconPathRole);
    if (a.installType    != b.installType)    roles.append(InstallTypeRole);
    if (a.isLoaded       != b.isLoaded)       roles.append(IsLoadedRole);
    if (a.isMainUi       != b.isMainUi)       roles.append(IsMainUiRole);
    if (a.hasMissingDeps != b.hasMissingDeps) roles.append(HasMissingDepsRole);
    // StatusText is derived from isMainUi/hasMissingDeps/isLoaded — refresh
    // it whenever any of those changed.
    if (a.isMainUi != b.isMainUi
        || a.hasMissingDeps != b.hasMissingDeps
        || a.isLoaded != b.isLoaded) {
        roles.append(StatusTextRole);
    }
    if (a.cpu    != b.cpu)    roles.append(CpuRole);
    if (a.memory != b.memory) roles.append(MemoryRole);
    return roles;
}

void ModuleInstanceModel::replaceRows(const QVariantList& installedModules)
{
    QList<Row> incoming;
    incoming.reserve(installedModules.size());
    for (const QVariant& v : installedModules) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("name")).toString().isEmpty()) continue;
        incoming.append(toRow(m));
    }

    // Fast path: same identity + same order → patch in place and emit
    // fine-grained dataChanged so QML delegates re-render only the moved
    // roles. This is what keeps the 2-second core-stats poll from resetting
    // the table (which would drop the selection and re-instantiate every
    // delegate — visible as a flicker).
    if (m_rows.size() == incoming.size()) {
        bool sameOrder = true;
        for (int i = 0; i < incoming.size(); ++i) {
            if (m_rows[i].name != incoming[i].name) { sameOrder = false; break; }
        }
        if (sameOrder) {
            for (int i = 0; i < incoming.size(); ++i) {
                const QList<int> changed = diffRoles(m_rows[i], incoming[i]);
                if (changed.isEmpty()) continue;
                m_rows[i] = incoming[i];
                const QModelIndex mi = index(i);
                emit dataChanged(mi, mi, changed);
            }
            return;
        }
    }

    // Row identities changed (add / remove / reorder) — a full reset is the
    // honest signal to consumers and keeps the code simple.
    beginResetModel();
    m_rows = std::move(incoming);
    endResetModel();
}
