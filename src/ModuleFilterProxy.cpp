#include "ModuleFilterProxy.h"

#include "ModuleModel.h"
#include "InstallEnums.h"

#include <climits>

ModuleFilterProxy::ModuleFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    sort(0);

    auto bump = [this]() {
        emit visibleCountChanged();
        emit installedCountChanged();
        emit breakdownChanged();
        emit hasResolutionErrorsChanged();
        emit totalDownloadBytesChanged();
    };
    connect(this, &QAbstractItemModel::rowsInserted,  this, bump);
    connect(this, &QAbstractItemModel::rowsRemoved,   this, bump);
    connect(this, &QAbstractItemModel::modelReset,    this, bump);
    connect(this, &QAbstractItemModel::dataChanged,   this, bump);
    connect(this, &QAbstractItemModel::layoutChanged, this, bump);
}

void ModuleFilterProxy::setSourceModel(QAbstractItemModel* sourceModel)
{
    QSortFilterProxyModel::setSourceModel(sourceModel);
    if (sourceModel) {
        connect(sourceModel, &QAbstractItemModel::rowsInserted,
                this, &ModuleFilterProxy::categoriesChanged);
        connect(sourceModel, &QAbstractItemModel::rowsRemoved,
                this, &ModuleFilterProxy::categoriesChanged);
        connect(sourceModel, &QAbstractItemModel::modelReset,
                this, &ModuleFilterProxy::categoriesChanged);
        connect(sourceModel, &QAbstractItemModel::dataChanged,
                this, &ModuleFilterProxy::categoriesChanged);
        // QSortFilterProxyModel only re-evaluates filterAcceptsRow on
        // dataChanged when filterRole() appears in the emitted roles list.
        // Our filter reads many custom roles (IsLoadedRole, TypeRole, …),
        // so force a re-evaluation whenever the source row changes.
        connect(sourceModel, &QAbstractItemModel::dataChanged,
                this, [this]() { invalidateRowsFilter(); });
    }
}

int ModuleFilterProxy::installedCount() const
{
    int c = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        const QModelIndex mi = index(i, 0);
        if (data(mi, ModuleModel::IsInstalledRole).toBool()
            || data(mi, ModuleModel::InstallStageRole).toInt()
                   == InstallStage::Installed) {
            ++c;
        }
    }
    return c;
}

int ModuleFilterProxy::installFreshCount() const
{
    int c = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        if (data(index(i, 0), ModuleModel::ActionRole).toString()
                == QStringLiteral("install")) ++c;
    }
    return c;
}

int ModuleFilterProxy::upgradeCount() const
{
    int c = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        if (data(index(i, 0), ModuleModel::ActionRole).toString()
                == QStringLiteral("upgrade")) ++c;
    }
    return c;
}

int ModuleFilterProxy::reinstallCount() const
{
    int c = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        if (data(index(i, 0), ModuleModel::ActionRole).toString()
                == QStringLiteral("reinstall")) ++c;
    }
    return c;
}

int ModuleFilterProxy::alreadyInstalledCount() const
{
    int c = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        if (data(index(i, 0), ModuleModel::ActionRole).toString()
                == QStringLiteral("installed")) ++c;
    }
    return c;
}

int ModuleFilterProxy::installingCount() const
{
    int c = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        if (data(index(i, 0), ModuleModel::ActionRole).toString()
                == QStringLiteral("installing")) ++c;
    }
    return c;
}

int ModuleFilterProxy::errorCount() const
{
    int c = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        if (data(index(i, 0), ModuleModel::ActionRole).toString()
                == QStringLiteral("error")) ++c;
    }
    return c;
}

qlonglong ModuleFilterProxy::totalDownloadBytes() const
{
    qlonglong total = 0;
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        const QModelIndex mi = index(i, 0);
        const QString action = data(mi, ModuleModel::ActionRole).toString();
        if (action != QStringLiteral("install")
            && action != QStringLiteral("upgrade")
            && action != QStringLiteral("downgrade")
            && action != QStringLiteral("reinstall")) {
            continue;
        }
        const QString toVersion = data(mi, ModuleModel::ToVersionRole).toString();
        const QVariantList versions = data(mi, ModuleModel::VersionsRole).toList();
        for (const QVariant& v : versions) {
            const QVariantMap entry = v.toMap();
            const QString entryVersion =
                entry.value("manifest").toMap().value("version").toString();
            if (entryVersion == toVersion) {
                total += entry.value("size").toLongLong();
                break;
            }
        }
    }
    return total;
}

bool ModuleFilterProxy::hasResolutionErrors() const
{
    const int n = rowCount();
    for (int i = 0; i < n; ++i) {
        const QModelIndex mi = index(i, 0);
        if (data(mi, ModuleModel::ActionRole).toString() == QStringLiteral("error"))
            return true;
    }
    return false;
}

QStringList ModuleFilterProxy::categories() const
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
            const QString nm = src->data(mi, ModuleModel::NameRole).toString();
            if (nm == QStringLiteral("main_ui")) continue;
        }
        if (!m_typeFilter.isEmpty()) {
            const QString t = src->data(mi, ModuleModel::TypeRole).toString();
            if (t != m_typeFilter) continue;
        }
        QString c = src->data(mi, ModuleModel::CategoryRole).toString();
        if (c.isEmpty()) continue;
        c[0] = c[0].toUpper();
        if (!seen.contains(c)) seen.append(c);
    }
    std::sort(seen.begin(), seen.end());
    out.append(seen);
    return out;
}

void ModuleFilterProxy::setTypeFilter(const QString& t)
{
    if (m_typeFilter == t) return;
    m_typeFilter = t;
    invalidateFilter();
    emit typeFilterChanged();
    emit categoriesChanged();
}

void ModuleFilterProxy::setCategoryFilter(const QString& c)
{
    if (m_categoryFilter == c) return;
    m_categoryFilter = c;
    invalidateFilter();
    emit categoryFilterChanged();
}

void ModuleFilterProxy::setSearchText(const QString& s)
{
    if (m_searchText == s) return;
    m_searchText = s;
    invalidateFilter();
    emit searchTextChanged();
}

void ModuleFilterProxy::setInstallStateFilter(const QString& s)
{
    if (m_installStateFilter == s) return;
    m_installStateFilter = s;
    invalidateFilter();
    emit installStateFilterChanged();
}

void ModuleFilterProxy::setExcludeMainUi(bool e)
{
    if (m_excludeMainUi == e) return;
    m_excludeMainUi = e;
    invalidateFilter();
    emit excludeMainUiChanged();
    emit categoriesChanged();
}

void ModuleFilterProxy::setExcludedNames(const QStringList& names)
{
    if (m_excludedNames == names) return;
    m_excludedNames = names;
    invalidateFilter();
    emit excludedNamesChanged();
}

void ModuleFilterProxy::setIsLoadedFilter(int value)
{
    if (m_isLoadedFilter == value) return;
    m_isLoadedFilter = value;
    invalidateFilter();
    emit isLoadedFilterChanged();
}

void ModuleFilterProxy::setRequireUiPluginRecord(bool value)
{
    if (m_requireUiPluginRecord == value) return;
    m_requireUiPluginRecord = value;
    invalidateFilter();
    emit requireUiPluginRecordChanged();
}

void ModuleFilterProxy::setRequireCoreModuleRecord(bool value)
{
    if (m_requireCoreModuleRecord == value) return;
    m_requireCoreModuleRecord = value;
    invalidateFilter();
    emit requireCoreModuleRecordChanged();
}

void ModuleFilterProxy::setRepositoryUrlFilter(const QString& url)
{
    if (m_repositoryUrlFilter == url) return;
    m_repositoryUrlFilter = url;
    invalidateFilter();
    emit repositoryUrlFilterChanged();
}

QStringList ModuleFilterProxy::requiredPackages() const
{
    QStringList out;
    out.resize(m_requiredPackagesOrder.size());
    for (auto it = m_requiredPackagesOrder.cbegin();
         it != m_requiredPackagesOrder.cend(); ++it) {
        if (it.value() < 0 || it.value() >= out.size()) continue;
        out[it.value()] = it.key();
    }
    return out;
}

void ModuleFilterProxy::setRequiredPackages(const QVariantList& entries)
{
    m_requiredPackagesActive = true;
    m_requiredPackagesByName.clear();
    m_requiredPackagesOrder.clear();
    int order = 0;
    for (const QVariant& v : entries) {
        const QVariantMap m = v.toMap();
        const QString name = m.value("name").toString();
        if (name.isEmpty()) continue;
        m_requiredPackagesByName.insert(name, m.value("repositoryUrl").toString());
        m_requiredPackagesOrder.insert(name, order++);
    }
    invalidate();
    emit requiredPackagesChanged();
}

QString ModuleFilterProxy::capitalizeFirst(const QString& s)
{
    if (s.isEmpty()) return s;
    return s.left(1).toUpper() + s.mid(1);
}

bool ModuleFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    QAbstractItemModel* src = sourceModel();
    if (!src) return false;
    const QModelIndex idx = src->index(sourceRow, 0, sourceParent);

    // main_ui exclusion — basecamp's own placeholder
    if (m_excludeMainUi) {
        const QString name = src->data(idx, ModuleModel::NameRole).toString();
        if (name == QStringLiteral("main_ui")) return false;
    }

    if (!m_excludedNames.isEmpty()) {
        const QString name = src->data(idx, ModuleModel::NameRole).toString();
        if (m_excludedNames.contains(name)) return false;
    }

    if (m_isLoadedFilter >= 0) {
        const bool loaded = src->data(idx, ModuleModel::IsLoadedRole).toBool();
        if (m_isLoadedFilter == 1 && !loaded) return false;
        if (m_isLoadedFilter == 0 && loaded) return false;
    }

    if (m_requireUiPluginRecord
        && !src->data(idx, ModuleModel::IsUiPluginRecordRole).toBool()) {
        return false;
    }

    if (m_requireCoreModuleRecord
        && !src->data(idx, ModuleModel::IsCoreModuleRecordRole).toBool()) {
        return false;
    }

    // Type filter.
    if (!m_typeFilter.isEmpty()) {
        const QString t = src->data(idx, ModuleModel::TypeRole).toString();
        if (t != m_typeFilter) return false;
    }

    // Category filter. "All" / "" both mean "no filter".
    if (!m_categoryFilter.isEmpty() && m_categoryFilter != QStringLiteral("All")) {
        const QString c = capitalizeFirst(
            src->data(idx, ModuleModel::CategoryRole).toString());
        if (c != m_categoryFilter) return false;
    }

    // Install-state filter.
    if (m_installStateFilter == QStringLiteral("installed")) {
        if (!src->data(idx, ModuleModel::IsInstalledRole).toBool()) return false;
    } else if (m_installStateFilter == QStringLiteral("notInstalled")) {
        if (src->data(idx, ModuleModel::IsInstalledRole).toBool()) return false;
    }

    // Search the visible fields — users type what they see, not the internal
    // package name. (DisplayNameRole falls back to name, so name search works.)
    if (!m_searchText.isEmpty()) {
        const QString n  = src->data(idx, ModuleModel::NameRole).toString();
        const QString dn = src->data(idx, ModuleModel::DisplayNameRole).toString();
        const QString ds = src->data(idx, ModuleModel::DescriptionRole).toString();
        if (!n.contains(m_searchText, Qt::CaseInsensitive)
            && !dn.contains(m_searchText, Qt::CaseInsensitive)
            && !ds.contains(m_searchText, Qt::CaseInsensitive))
            return false;
    }

    // Repository URL — exact match. Used by the App Manager's per-repo
    // sections.
    if (!m_repositoryUrlFilter.isEmpty()) {
        const QString repo = src->data(idx, ModuleModel::RepositoryUrlRole).toString();
        if (repo != m_repositoryUrlFilter) return false;
    }

    // requiredPackages: name in map AND (pinned repo empty OR matches row).
    // Without the repo pin, two repos publishing the same name both pass.
    if (m_requiredPackagesActive) {
        const QString n = src->data(idx, ModuleModel::NameRole).toString();
        const auto it = m_requiredPackagesByName.constFind(n);
        if (it == m_requiredPackagesByName.constEnd()) return false;
        if (!it.value().isEmpty()) {
            const QString rowRepo =
                src->data(idx, ModuleModel::RepositoryUrlRole).toString();
            if (rowRepo != it.value()) return false;
        }
    }

    return true;
}

bool ModuleFilterProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    if (m_requiredPackagesActive) {
        const QString ln = sourceModel()->data(left,  ModuleModel::NameRole).toString();
        const QString rn = sourceModel()->data(right, ModuleModel::NameRole).toString();
        return m_requiredPackagesOrder.value(ln, INT_MAX)
             < m_requiredPackagesOrder.value(rn, INT_MAX);
    }
    return QSortFilterProxyModel::lessThan(left, right);
}
