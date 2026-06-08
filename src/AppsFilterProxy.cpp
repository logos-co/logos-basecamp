#include "AppsFilterProxy.h"

#include "AppsModel.h"
#include "InstallStage.h"

#include <climits>

AppsFilterProxy::AppsFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    sort(0);

    auto bump = [this]() {
        emit visibleCountChanged();
        emit installedCountChanged();
        emit hasResolutionErrorsChanged();
        emit totalDownloadBytesChanged();
    };
    connect(this, &QAbstractItemModel::rowsInserted,  this, bump);
    connect(this, &QAbstractItemModel::rowsRemoved,   this, bump);
    connect(this, &QAbstractItemModel::modelReset,    this, bump);
    connect(this, &QAbstractItemModel::dataChanged,   this, bump);
    connect(this, &QAbstractItemModel::layoutChanged, this, bump);
}

void AppsFilterProxy::setSourceModel(QAbstractItemModel* sourceModel)
{
    QSortFilterProxyModel::setSourceModel(sourceModel);
    if (sourceModel) {
        connect(sourceModel, &QAbstractItemModel::rowsInserted,
                this, &AppsFilterProxy::categoriesChanged);
        connect(sourceModel, &QAbstractItemModel::rowsRemoved,
                this, &AppsFilterProxy::categoriesChanged);
        connect(sourceModel, &QAbstractItemModel::modelReset,
                this, &AppsFilterProxy::categoriesChanged);
        connect(sourceModel, &QAbstractItemModel::dataChanged,
                this, &AppsFilterProxy::categoriesChanged);
    }
}

int AppsFilterProxy::installedCount() const
{
    int c = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        const QModelIndex mi = index(i, 0);
        if (data(mi, AppsModel::IsInstalledRole).toBool()
            || data(mi, AppsModel::InstallStageRole).toInt()
                   == InstallStage::Installed) {
            ++c;
        }
    }
    return c;
}

qlonglong AppsFilterProxy::totalDownloadBytes() const
{
    qlonglong total = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        const QModelIndex mi = index(i, 0);
        const QString action = data(mi, AppsModel::ActionRole).toString();
        if (action != QStringLiteral("install")
            && action != QStringLiteral("upgrade")
            && action != QStringLiteral("downgrade")) {
            continue;
        }
        const QString toVersion = data(mi, AppsModel::ToVersionRole).toString();
        const QVariantList versions = data(mi, AppsModel::VersionsRole).toList();
        for (const QVariant& v : versions) {
            const QVariantMap entry = v.toMap();
            if (entry.value("version").toString() == toVersion) {
                total += entry.value("size").toLongLong();
                break;
            }
        }
    }
    return total;
}

bool AppsFilterProxy::hasResolutionErrors() const
{
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        const QModelIndex mi = index(i, 0);
        if (data(mi, AppsModel::ActionRole).toString() == QStringLiteral("error"))
            return true;
    }
    return false;
}

QStringList AppsFilterProxy::categories() const
{
    QAbstractItemModel* src = sourceModel();
    QStringList out;
    out.append(QStringLiteral("All"));
    if (!src) return out;

    QStringList seen;
    const int n = src->rowCount();
    for (int i = 0; i < n; ++i) {
        const QModelIndex mi = src->index(i, 0);
        if (m_excludeMainUi) {
            const QString nm = src->data(mi, AppsModel::NameRole).toString();
            if (nm == QStringLiteral("main_ui")) continue;
        }
        if (!m_typeFilter.isEmpty()) {
            const QString t = src->data(mi, AppsModel::TypeRole).toString();
            if (t != m_typeFilter) continue;
        }
        QString c = src->data(mi, AppsModel::CategoryRole).toString();
        if (c.isEmpty()) continue;
        c[0] = c[0].toUpper();
        if (!seen.contains(c)) seen.append(c);
    }
    std::sort(seen.begin(), seen.end());
    out.append(seen);
    return out;
}

void AppsFilterProxy::setTypeFilter(const QString& t)
{
    if (m_typeFilter == t) return;
    m_typeFilter = t;
    invalidateFilter();
    emit typeFilterChanged();
    emit categoriesChanged();
}

void AppsFilterProxy::setCategoryFilter(const QString& c)
{
    if (m_categoryFilter == c) return;
    m_categoryFilter = c;
    invalidateFilter();
    emit categoryFilterChanged();
}

void AppsFilterProxy::setSearchText(const QString& s)
{
    if (m_searchText == s) return;
    m_searchText = s;
    invalidateFilter();
    emit searchTextChanged();
}

void AppsFilterProxy::setInstallStateFilter(const QString& s)
{
    if (m_installStateFilter == s) return;
    m_installStateFilter = s;
    invalidateFilter();
    emit installStateFilterChanged();
}

void AppsFilterProxy::setExcludeMainUi(bool e)
{
    if (m_excludeMainUi == e) return;
    m_excludeMainUi = e;
    invalidateFilter();
    emit excludeMainUiChanged();
    emit categoriesChanged();
}

void AppsFilterProxy::setRequiredPackages(const QStringList& names)
{
    const bool wasActive = m_requiredPackagesActive;
    m_requiredPackagesActive = true;
    if (wasActive && m_requiredPackages == names) return;
    m_requiredPackages = names;
    m_requiredPackagesIndex.clear();
    for (int i = 0; i < names.size(); ++i)
        m_requiredPackagesIndex.insert(names[i], i);
    invalidate();
    emit requiredPackagesChanged();
}

QString AppsFilterProxy::capitalizeFirst(const QString& s)
{
    if (s.isEmpty()) return s;
    return s.left(1).toUpper() + s.mid(1);
}

bool AppsFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    QAbstractItemModel* src = sourceModel();
    if (!src) return false;
    const QModelIndex idx = src->index(sourceRow, 0, sourceParent);

    // main_ui exclusion — basecamp's own placeholder
    if (m_excludeMainUi) {
        const QString name = src->data(idx, AppsModel::NameRole).toString();
        if (name == QStringLiteral("main_ui")) return false;
    }

    // Type filter.
    if (!m_typeFilter.isEmpty()) {
        const QString t = src->data(idx, AppsModel::TypeRole).toString();
        if (t != m_typeFilter) return false;
    }

    // Category filter. "All" / "" both mean "no filter".
    if (!m_categoryFilter.isEmpty() && m_categoryFilter != QStringLiteral("All")) {
        const QString c = capitalizeFirst(
            src->data(idx, AppsModel::CategoryRole).toString());
        if (c != m_categoryFilter) return false;
    }

    // Install-state filter.
    if (m_installStateFilter == QStringLiteral("installed")) {
        if (!src->data(idx, AppsModel::IsInstalledRole).toBool()) return false;
    } else if (m_installStateFilter == QStringLiteral("notInstalled")) {
        if (src->data(idx, AppsModel::IsInstalledRole).toBool()) return false;
    }

    // Search text (case-insensitive substring on name).
    if (!m_searchText.isEmpty()) {
        const QString n = src->data(idx, AppsModel::NameRole).toString();
        if (!n.contains(m_searchText, Qt::CaseInsensitive)) return false;
    }

    // requiredPackages membership (dialog use). The dialog proxy flips
    // m_requiredPackagesActive on its first setRequiredPackages call;
    // once active, only rows in m_requiredPackagesIndex pass
    if (m_requiredPackagesActive) {
        const QString n = src->data(idx, AppsModel::NameRole).toString();
        if (!m_requiredPackagesIndex.contains(n)) return false;
    }

    return true;
}

bool AppsFilterProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    if (m_requiredPackagesActive) {
        const QString ln = sourceModel()->data(left,  AppsModel::NameRole).toString();
        const QString rn = sourceModel()->data(right, AppsModel::NameRole).toString();
        return m_requiredPackagesIndex.value(ln, INT_MAX)
             < m_requiredPackagesIndex.value(rn, INT_MAX);
    }
    return QSortFilterProxyModel::lessThan(left, right);
}
