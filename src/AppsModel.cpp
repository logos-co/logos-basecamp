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
    case IsInstalledRole:      return !r.installedVersion.isEmpty()
                                      && r.missingDeps.isEmpty();
    case MissingDepsRole:      return r.missingDeps;
    case InstallStatusRole:    return static_cast<int>(r.installStatus);
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
        {MissingDepsRole,      "missingDeps"},
        {InstallStatusRole,    "installStatus"},
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

// Dotted-numeric compare. Mirrors PMUI's versionCmp at
// repos/logos-package-manager-ui/src/RowActionResolver.h:31 so the App
// Manager grid sees the same Upgrade/Downgrade verdict the Package
// Manager view does. Kept file-local to avoid a header dependency
// across repos.
static int versionCmp(const QString& a, const QString& b)
{
    const QStringList aParts = a.split('.');
    const QStringList bParts = b.split('.');
    const int n = std::max(aParts.size(), bParts.size());
    for (int i = 0; i < n; ++i) {
        const int av = (i < aParts.size()) ? aParts[i].toInt() : 0;
        const int bv = (i < bParts.size()) ? bParts[i].toInt() : 0;
        if (av < bv) return -1;
        if (av > bv) return  1;
    }
    return 0;
}

void AppsModel::recomputeInstallStatus(Row& r)
{
    // Mirrors PMUI's RowActionResolver verdict (Install/Installed/Upgrade/
    // Downgrade/Reinstall) and extends it with per-dep hash checks so two
    // repos publishing the same top-level rootHash but different dep hashes
    // don't both read Installed.
    if (r.installedVersion.isEmpty()) {
        r.installStatus = InstallStatus::NotInstalled;
        return;
    }
    if (!r.missingDeps.isEmpty()) {
        // Partially installed → tile reads Install, click goes through the
        // resolver and fills in the missing pieces.
        r.installStatus = InstallStatus::NotInstalled;
        return;
    }
    const QString releaseVersion = r.latestVersion;
    const QString releaseHash    = r.versions.isEmpty()
        ? QString()
        : r.versions.first().toMap().value("rootHash").toString();
    if (releaseVersion.isEmpty()) {
        // No catalog version to compare against — best-effort Installed.
        r.installStatus = InstallStatus::Installed;
        return;
    }
    const int cmp = versionCmp(r.installedVersion, releaseVersion);
    if (cmp < 0) { r.installStatus = InstallStatus::UpgradeAvailable;   return; }
    if (cmp > 0) { r.installStatus = InstallStatus::DowngradeAvailable; return; }
    if (!releaseHash.isEmpty() && !r.installedHash.isEmpty()
        && releaseHash != r.installedHash) {
        r.installStatus = InstallStatus::DifferentHash;
        return;
    }
    // Same version + same (or unknown) top-level hash. A dep with a hash
    // mismatch against this repo's catalog row demotes the tile to
    // DifferentHash. Intentional: the lookup is scoped to THIS repo, not
    // any repo — if a repo doesn't catalogue one of its declared deps,
    // we trust whatever's on disk rather than punishing the tile with
    // DifferentHash based on another repo's expected value.
    for (const QVariant& v : r.dependencies) {
        const QVariantMap dep = v.toMap();
        const QString depName = dep.value("name").toString();
        if (depName.isEmpty()) continue;
        const int depIdx = rowOf(depName, r.repositoryUrl);
        if (depIdx < 0) continue;   // dep not in this repo — see comment above
        const Row& depRow = m_rows[depIdx];
        if (depRow.versions.isEmpty()) continue;
        const QString expectedDepHash =
            depRow.versions.first().toMap().value("rootHash").toString();
        const QString installedDepHash = depRow.installedHash;
        if (expectedDepHash.isEmpty() || installedDepHash.isEmpty()) continue;
        if (expectedDepHash != installedDepHash) {
            r.installStatus = InstallStatus::DifferentHash;
            return;
        }
    }
    r.installStatus = InstallStatus::Installed;
}

void AppsModel::recomputeVersionDerivedFields(Row& r)
{
    QVariantMap firstManifest;
    if (!r.versions.isEmpty()) {
        firstManifest = r.versions.first().toMap().value("manifest").toMap();
    }
    r.latestVersion = firstManifest.value("version").toString();

    r.dependencies.clear();
    if (!r.versions.isEmpty()) {
        const QVariantList raw = firstManifest.value("dependencies").toList();
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
    recomputeInstallStatus(r);
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
                DependenciesRole, InstallStatusRole
            });
        }
    }
    emit categoriesChanged();
}

// ── Mutation: on-disk state ────────────────────────────────────────────────

void AppsModel::markInstalled(const QString& name,
                              const QString& installedVersion,
                              const QString& installedHash)
{
    bool anyChanged = false;
    for (int idx = 0; idx < m_rows.size(); ++idx) {
        if (m_rows[idx].name != name) continue;
        Row& r = m_rows[idx];
        if (r.installedVersion == installedVersion
            && r.installedHash == installedHash) continue;
        r.installedVersion = installedVersion;
        r.installedHash    = installedHash;
        recomputeInstallStatus(r);
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {InstalledVersionRole, HasUpdateRole,
                                  IsInstalledRole, InstallStatusRole});
        anyChanged = true;
    }
    if (!anyChanged) return;

    for (int idx = 0; idx < m_rows.size(); ++idx) {
        Row& other = m_rows[idx];
        if (other.name == name) continue;
        const InstallStatus::Value prev = other.installStatus;
        recomputeInstallStatus(other);
        if (other.installStatus != prev) {
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, {InstallStatusRole, IsInstalledRole});
        }
    }
}

void AppsModel::setInstallType(const QString& name, const QString& installType)
{
    for (int idx = 0; idx < m_rows.size(); ++idx) {
        if (m_rows[idx].name != name) continue;
        Row& r = m_rows[idx];
        if (r.installType == installType) continue;
        r.installType = installType;
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {InstallTypeRole});
    }
}

void AppsModel::setIconUrl(const QString& name, const QString& iconUrl)
{
    for (int idx = 0; idx < m_rows.size(); ++idx) {
        if (m_rows[idx].name != name) continue;
        Row& r = m_rows[idx];
        if (r.iconUrl == iconUrl) continue;
        r.iconUrl = iconUrl;
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {IconUrlRole});
    }
}

void AppsModel::setMissingDeps(const QString& name, const QStringList& missing)
{
    for (int idx = 0; idx < m_rows.size(); ++idx) {
        if (m_rows[idx].name != name) continue;
        Row& r = m_rows[idx];
        if (r.missingDeps == missing) continue;
        r.missingDeps = missing;
        recomputeInstallStatus(r);
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {MissingDepsRole, IsInstalledRole, InstallStatusRole});
    }
}

// ── Mutation: live install stage ──────────────────────────────────────────

void AppsModel::setInstallStage(const QString& name,
                                InstallStage::Value stage,
                                const QString& error)
{
    for (int idx = 0; idx < m_rows.size(); ++idx) {
        if (m_rows[idx].name != name) continue;
        Row& r = m_rows[idx];
        if (r.installStage == stage && r.installError == error) continue;
        r.installStage = stage;
        r.installError = stage == InstallStage::Failed ? error : QString();
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {InstallStageRole, InstallErrorRole});
    }
}

// ── Mutation: resolver overlay ─────────────────────────────────────────────

void AppsModel::setResolverOverlay(const QList<ResolverRow>& rows)
{
    clearResolverOverlay();
    for (const ResolverRow& src : rows) {
        const int idx = rowOf(src.name, src.repositoryUrl);
        if (idx < 0) continue;
        Row& r = m_rows[idx];
        r.action        = src.action;
        r.toVersion     = src.toVersion;
        r.isTopLevel    = src.isTopLevel;
        r.resolverError = src.resolverError;
        // Wipe any sticky pipeline state from a previous session
        QList<int> changedRoles{ActionRole, ToVersionRole, IsTopLevelRole, ResolverErrorRole};
        if (r.installStage != InstallStage::None
            || !r.installError.isEmpty()) {
            r.installStage = InstallStage::None;
            r.installError.clear();
            changedRoles << InstallStageRole << InstallErrorRole;
        }
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, changedRoles);
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
