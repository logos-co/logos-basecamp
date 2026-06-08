#include "AppsModel.h"

#include <QSet>

namespace {
constexpr QChar kSep = QLatin1Char('\n');
}

QString AppsModel::key(const QString& repo, const QString& name)
{
    return repo + kSep + name;
}

AppsModel::AppsModel(QObject* parent) : QAbstractListModel(parent) {}

// ── QAbstractListModel ─────────────────────────────────────────────────────

int AppsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant AppsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row& r = m_rows[index.row()];
    switch (role) {
    case NameRole:             return r.name;
    case RepositoryUrlRole:    return r.repositoryUrl;
    case DisplayNameRole:      return r.displayName.isEmpty() ? r.name : r.displayName;
    case DescriptionRole:      return r.description;
    case CategoryRole:         return r.category;
    case TypeRole:             return r.type;
    case IconUrlRole:          return r.iconUrl;
    case VersionsRole:         return r.versions;
    case DependenciesRole:     return r.dependencies;
    case InstalledVersionRole: return r.installedVersion;
    case LatestVersionRole:    return r.latestVersion;
    case HasUpdateRole:
        return !r.installedVersion.isEmpty()
            && !r.latestVersion.isEmpty()
            && r.installedVersion != r.latestVersion;
    case IsInstalledRole:      return !r.installedVersion.isEmpty();
    case InstallTypeRole:      return r.installType;
    case ActionRole:           return r.action;
    case ToVersionRole:        return r.toVersion;
    case IsTopLevelRole:       return r.isTopLevel;
    case ResolverErrorRole:    return r.resolverError;
    case InstallStageRole:     return static_cast<int>(r.installStage);
    case InstallErrorRole:     return r.installError;
    }
    return {};
}

QHash<int, QByteArray> AppsModel::roleNames() const
{
    return {
        {NameRole,             "name"},
        {RepositoryUrlRole,    "repositoryUrl"},
        {DisplayNameRole,      "displayName"},
        {DescriptionRole,      "description"},
        {CategoryRole,         "category"},
        {TypeRole,             "type"},
        {IconUrlRole,          "iconUrl"},
        {VersionsRole,         "versions"},
        {DependenciesRole,     "dependencies"},
        {InstalledVersionRole, "installedVersion"},
        {LatestVersionRole,    "latestVersion"},
        {HasUpdateRole,        "hasUpdate"},
        {IsInstalledRole,      "isInstalled"},
        {InstallTypeRole,      "installType"},
        {ActionRole,           "action"},
        {ToVersionRole,        "toVersion"},
        {IsTopLevelRole,       "isTopLevel"},
        {ResolverErrorRole,    "resolverError"},
        {InstallStageRole,     "installStage"},
        {InstallErrorRole,     "installError"},
    };
}

QStringList AppsModel::categories() const
{
    QStringList seen;
    seen.append(QStringLiteral("All"));
    for (const Row& r : m_rows) {
        if (r.category.isEmpty()) continue;
        QString c = r.category;
        c[0] = c[0].toUpper();
        if (!seen.contains(c)) seen.append(c);
    }
    std::sort(seen.begin() + 1, seen.end());   // keep "All" first
    return seen;
}

// ── Helpers ────────────────────────────────────────────────────────────────

void AppsModel::recomputeVersionDerivedFields(Row& r)
{
    r.latestVersion = r.versions.isEmpty()
        ? QString()
        : r.versions.first().toMap().value("version").toString();

    r.dependencies.clear();
    if (r.versions.isEmpty()) return;
    const QVariantMap manifest =
        r.versions.first().toMap().value("manifest").toMap();
    const QVariantList raw = manifest.value("dependencies").toList();
    for (const QVariant& v : raw) {
        QVariantMap entry;
        if (v.typeId() == QMetaType::QString) {
            entry["name"]    = v.toString();
            entry["version"] = QString();
        } else {
            const QVariantMap m = v.toMap();
            entry["name"]    = m.value("name").toString();
            entry["version"] = m.value("version").toString();
        }
        if (entry.value("name").toString().isEmpty()) continue;
        r.dependencies.append(entry);
    }
}

// ── Mutation: bulk replace from catalog ────────────────────────────────────

void AppsModel::replaceCatalog(const QVariantList& catalogRows)
{
    QSet<QString> incoming;
    incoming.reserve(catalogRows.size());
    for (const QVariant& v : catalogRows) {
        const QVariantMap row = v.toMap();
        const QString name = row.value("name").toString();
        if (name.isEmpty()) continue;
        incoming.insert(key(row.value("repositoryUrl").toString(), name));
    }

    QList<int> toRemove;
    for (int i = 0; i < m_rows.size(); ++i) {
        const QString k = key(m_rows[i].repositoryUrl, m_rows[i].name);
        if (!incoming.contains(k)) toRemove.append(i);
    }
    for (int i = toRemove.size() - 1; i >= 0; --i) {
        const int idx = toRemove[i];
        beginRemoveRows({}, idx, idx);
        const QString k = key(m_rows[idx].repositoryUrl, m_rows[idx].name);
        m_indexByKey.remove(k);
        m_rows.removeAt(idx);
        endRemoveRows();
    }
    m_indexByKey.clear();
    for (int i = 0; i < m_rows.size(); ++i)
        m_indexByKey.insert(key(m_rows[i].repositoryUrl, m_rows[i].name), i);

    // Upsert incoming rows.
    for (const QVariant& v : catalogRows) {
        const QVariantMap row = v.toMap();
        const QString name = row.value("name").toString();
        if (name.isEmpty()) continue;
        const QString repo = row.value("repositoryUrl").toString();
        const QString k = key(repo, name);

        const auto it = m_indexByKey.find(k);
        if (it == m_indexByKey.end()) {
            // Insert new row at the end.
            const int idx = m_rows.size();
            beginInsertRows({}, idx, idx);
            Row r;
            r.name           = name;
            r.repositoryUrl  = repo;
            r.displayName    = row.value("displayName").toString();
            r.description    = row.value("description").toString();
            r.category       = row.value("category").toString();
            r.type           = row.value("type").toString();
            r.iconUrl        = row.value("iconUrl").toString();
            r.versions       = row.value("versions").toList();
            recomputeVersionDerivedFields(r);
            m_rows.append(std::move(r));
            m_indexByKey.insert(k, idx);
            endInsertRows();
        } else {
            // Update catalog fields on existing row, preserve everything else.
            const int idx = it.value();
            Row& r = m_rows[idx];
            r.displayName = row.value("displayName").toString();
            r.description = row.value("description").toString();
            r.category    = row.value("category").toString();
            r.type        = row.value("type").toString();
            r.iconUrl     = row.value("iconUrl").toString();
            r.versions    = row.value("versions").toList();
            recomputeVersionDerivedFields(r);
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, {
                DisplayNameRole, DescriptionRole, CategoryRole, TypeRole,
                IconUrlRole, VersionsRole, LatestVersionRole, HasUpdateRole,
                DependenciesRole
            });
        }
    }
    emit categoriesChanged();
}

// ── Mutation: on-disk state ────────────────────────────────────────────────

void AppsModel::markInstalled(const QString& name, const QString& installedVersion)
{
    const int idx = rowOf(name);
    if (idx < 0) return;
    Row& r = m_rows[idx];
    if (r.installedVersion == installedVersion) return;
    r.installedVersion = installedVersion;
    // HasUpdate is derived; emit both. Also IsInstalled.
    const QModelIndex mi = index(idx);
    emit dataChanged(mi, mi, {InstalledVersionRole, HasUpdateRole, IsInstalledRole});
}

void AppsModel::setInstallType(const QString& name, const QString& installType)
{
    const int idx = rowOf(name);
    if (idx < 0) return;
    Row& r = m_rows[idx];
    if (r.installType == installType) return;
    r.installType = installType;
    const QModelIndex mi = index(idx);
    emit dataChanged(mi, mi, {InstallTypeRole});
}

void AppsModel::setIconUrl(const QString& name, const QString& iconUrl)
{
    const int idx = rowOf(name);
    if (idx < 0) return;
    Row& r = m_rows[idx];
    if (r.iconUrl == iconUrl) return;
    r.iconUrl = iconUrl;
    const QModelIndex mi = index(idx);
    emit dataChanged(mi, mi, {IconUrlRole});
}

// ── Mutation: live install stage ──────────────────────────────────────────

void AppsModel::setInstallStage(const QString& name,
                                InstallStage::Value stage,
                                const QString& error)
{
    const int idx = rowOf(name);
    if (idx < 0) return;
    Row& r = m_rows[idx];
    if (r.installStage == stage && r.installError == error) return;
    r.installStage = stage;
    r.installError = stage == InstallStage::Failed ? error : QString();
    const QModelIndex mi = index(idx);
    emit dataChanged(mi, mi, {InstallStageRole, InstallErrorRole});
}

// ── Mutation: resolver overlay ─────────────────────────────────────────────

void AppsModel::setResolverOverlay(const QList<ResolverRow>& rows)
{
    clearResolverOverlay();
    for (const ResolverRow& src : rows) {
        const int idx = rowOf(src.name);
        if (idx < 0) continue;
        Row& r = m_rows[idx];
        r.action        = src.action;
        r.toVersion     = src.toVersion;
        r.isTopLevel    = src.isTopLevel;
        r.resolverError = src.resolverError;
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {ActionRole, ToVersionRole, IsTopLevelRole, ResolverErrorRole});
    }
}

void AppsModel::clearResolverOverlay()
{
    for (int i = 0; i < m_rows.size(); ++i) {
        Row& r = m_rows[i];
        if (r.action.isEmpty() && r.toVersion.isEmpty()
            && !r.isTopLevel && r.resolverError.isEmpty())
            continue;
        r.action.clear();
        r.toVersion.clear();
        r.isTopLevel = false;
        r.resolverError.clear();
        const QModelIndex mi = index(i);
        emit dataChanged(mi, mi, {ActionRole, ToVersionRole, IsTopLevelRole, ResolverErrorRole});
    }
}

// ── Lookup ─────────────────────────────────────────────────────────────────

int AppsModel::rowOf(const QString& name, const QString& repositoryUrl) const
{
    if (!repositoryUrl.isEmpty()) {
        const auto it = m_indexByKey.find(key(repositoryUrl, name));
        return it == m_indexByKey.end() ? -1 : it.value();
    }
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].name == name) return i;
    return -1;
}

QVariantMap AppsModel::rowData(int row) const
{
    if (row < 0 || row >= m_rows.size()) return {};
    const QModelIndex mi = index(row);
    QVariantMap m;
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        m.insert(QString::fromUtf8(it.value()), data(mi, it.key()));
    return m;
}

QVariantMap AppsModel::rowDataByName(const QString& name,
                                     const QString& repositoryUrl) const
{
    return rowData(rowOf(name, repositoryUrl));
}
