#include "ModulesFilterProxy.h"

#include "ModuleInstanceModel.h"

#include <QAbstractItemModel>
#include <QByteArray>
#include <QHash>
#include <QModelIndex>
#include <QVariant>

ModulesFilterProxy::ModulesFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    // Kept in sync with m_sortRoleName in setSourceModel/setSortRoleName so
    // the initial sort works before either setter is called.
    sort(0, Qt::AscendingOrder);

    // rowCount changes when the underlying model does OR when filters flip —
    // fold both into visibleCount so the header's "N of M" readout stays
    // live.
    connect(this, &QAbstractItemModel::rowsInserted,
            this, &ModulesFilterProxy::visibleCountChanged);
    connect(this, &QAbstractItemModel::rowsRemoved,
            this, &ModulesFilterProxy::visibleCountChanged);
    connect(this, &QAbstractItemModel::modelReset,
            this, &ModulesFilterProxy::visibleCountChanged);
    connect(this, &QAbstractItemModel::layoutChanged,
            this, &ModulesFilterProxy::visibleCountChanged);
}

int ModulesFilterProxy::totalCount() const
{
    QAbstractItemModel* src = sourceModel();
    return src ? src->rowCount() : 0;
}

void ModulesFilterProxy::setSourceModel(QAbstractItemModel* sourceModel)
{
    if (QAbstractItemModel* prev = this->sourceModel()) prev->disconnect(this);

    QSortFilterProxyModel::setSourceModel(sourceModel);
    setSortRole(roleFromName(m_sortRoleName.toUtf8()));

    if (sourceModel) {
        // Source row-count changes also move totalCount ("M" in "N of M").
        connect(sourceModel, &QAbstractItemModel::rowsInserted,
                this, &ModulesFilterProxy::totalCountChanged);
        connect(sourceModel, &QAbstractItemModel::rowsRemoved,
                this, &ModulesFilterProxy::totalCountChanged);
        connect(sourceModel, &QAbstractItemModel::modelReset,
                this, &ModulesFilterProxy::totalCountChanged);
    }
    emit totalCountChanged();
    emit visibleCountChanged();
}

void ModulesFilterProxy::setSearchText(const QString& s)
{
    if (s == m_searchText) return;
    m_searchText = s;
    invalidateFilter();
    emit searchTextChanged();
    emit visibleCountChanged();
}

void ModulesFilterProxy::setStateFilter(const QString& s)
{
    if (s == m_stateFilter) return;
    m_stateFilter = s;
    invalidateFilter();
    emit stateFilterChanged();
    emit visibleCountChanged();
}

void ModulesFilterProxy::setSortRoleName(const QString& name)
{
    if (name == m_sortRoleName) return;
    m_sortRoleName = name;
    setSortRole(roleFromName(name.toUtf8()));
    // setSortRole doesn't re-sort by itself.
    invalidate();
    emit sortRoleNameChanged();
}

int ModulesFilterProxy::roleFromName(const QByteArray& name) const
{
    QAbstractItemModel* src = sourceModel();
    if (!src) return ModuleInstanceModel::LabelRole;
    const auto roles = src->roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if (it.value() == name) return it.key();
    }
    return ModuleInstanceModel::LabelRole;
}

bool ModulesFilterProxy::filterAcceptsRow(int sourceRow,
                                          const QModelIndex& sourceParent) const
{
    QAbstractItemModel* src = sourceModel();
    if (!src) return true;

    const QModelIndex idx = src->index(sourceRow, 0, sourceParent);

    // State filter.
    if (m_stateFilter == QLatin1String("loaded")
        && !src->data(idx, ModuleInstanceModel::IsLoadedRole).toBool()) return false;
    if (m_stateFilter == QLatin1String("notLoaded")
        && src->data(idx, ModuleInstanceModel::IsLoadedRole).toBool()) return false;

    // Search filter — trimmed + lowercased against a fixed set of textual
    // roles. Empty needle accepts everything.
    const QString needle = m_searchText.trimmed().toLower();
    if (needle.isEmpty()) return true;

    static const int textRoles[] = {
        ModuleInstanceModel::NameRole,
        ModuleInstanceModel::LabelRole,
        ModuleInstanceModel::StatusTextRole,
        ModuleInstanceModel::DescriptionRole,
        ModuleInstanceModel::VersionRole,
    };
    for (int role : textRoles) {
        if (src->data(idx, role).toString().toLower().contains(needle))
            return true;
    }
    return false;
}

bool ModulesFilterProxy::lessThan(const QModelIndex& left,
                                  const QModelIndex& right) const
{
    QAbstractItemModel* src = sourceModel();
    if (!src) return false;

    const int role = sortRole();
    const QVariant lv = src->data(left, role);
    const QVariant rv = src->data(right, role);

    int result = 0;
    if (lv.typeId() == QMetaType::Double || rv.typeId() == QMetaType::Double
        || lv.typeId() == QMetaType::Int || rv.typeId() == QMetaType::Int) {
        // Numeric roles (cpu, memory) — compare as doubles even if one side
        // came in as int.
        const double ld = lv.toDouble();
        const double rd = rv.toDouble();
        if      (ld < rd) result = -1;
        else if (ld > rd) result =  1;
    } else if (lv.typeId() == QMetaType::Bool || rv.typeId() == QMetaType::Bool) {
        result = (lv.toBool() ? 1 : 0) - (rv.toBool() ? 1 : 0);
    } else {
        result = lv.toString().localeAwareCompare(rv.toString());
    }

    // Stable tie-break by name so equal-status / equal-stat rows don't shuffle
    // between refreshes (the core stats poll fires every 2s).
    if (result == 0 && role != ModuleInstanceModel::NameRole) {
        result = src->data(left,  ModuleInstanceModel::NameRole).toString()
                    .localeAwareCompare(
                 src->data(right, ModuleInstanceModel::NameRole).toString());
    }
    return result < 0;
}
