#include "AppsFilterProxy.h"

#include "BasecampModelRoles.h"

AppsFilterProxy::AppsFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    sort(0);

    auto bump = [this]() { emit visibleCountChanged(); };
    connect(this, &QAbstractItemModel::rowsInserted,  this, bump);
    connect(this, &QAbstractItemModel::rowsRemoved,   this, bump);
    connect(this, &QAbstractItemModel::modelReset,    this, bump);
    connect(this, &QAbstractItemModel::layoutChanged, this, bump);
}

void AppsFilterProxy::setTypeFilter(const QString& t)
{
    if (m_typeFilter == t) return;
    m_typeFilter = t;
    invalidateFilter();
    emit typeFilterChanged();
}

void AppsFilterProxy::setSearchText(const QString& s)
{
    if (m_searchText == s) return;
    m_searchText = s;
    invalidateFilter();
    emit searchTextChanged();
}

void AppsFilterProxy::setExcludeMainUi(bool e)
{
    if (m_excludeMainUi == e) return;
    m_excludeMainUi = e;
    invalidateFilter();
    emit excludeMainUiChanged();
}

bool AppsFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    QAbstractItemModel* src = sourceModel();
    if (!src) return false;
    const QModelIndex idx = src->index(sourceRow, 0, sourceParent);

    // main_ui exclusion — basecamp's own placeholder
    if (m_excludeMainUi) {
        const QString name = src->data(idx, AppsModelRoles::NameRole).toString();
        if (name == QStringLiteral("main_ui")) return false;
    }

    // Type filter.
    if (!m_typeFilter.isEmpty()) {
        const QString t = src->data(idx, AppsModelRoles::TypeRole).toString();
        if (t != m_typeFilter) return false;
    }

    if (!src->data(idx, AppsModelRoles::HasInstallableVersionRole).toBool()
        && !src->data(idx, AppsModelRoles::IsInstalledRole).toBool()) {
        return false;
    }

    // Search the visible fields — users type what they see, not the internal
    // package name. (DisplayNameRole falls back to name, so name search works.)
    if (!m_searchText.isEmpty()) {
        const QString n  = src->data(idx, AppsModelRoles::NameRole).toString();
        const QString dn = src->data(idx, AppsModelRoles::DisplayNameRole).toString();
        const QString ds = src->data(idx, AppsModelRoles::DescriptionRole).toString();
        if (!n.contains(m_searchText, Qt::CaseInsensitive)
            && !dn.contains(m_searchText, Qt::CaseInsensitive)
            && !ds.contains(m_searchText, Qt::CaseInsensitive))
            return false;
    }

    return true;
}
