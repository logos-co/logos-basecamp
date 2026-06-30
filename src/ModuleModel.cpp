#include "ModuleModel.h"

#include "InstallRegistry.h"

#include <QDebug>
#include <QSet>

namespace {
constexpr QChar kSep = QLatin1Char('\n');

bool isUiPluginType(const QString& type)
{
    return type == QStringLiteral("ui_qml") || type == QStringLiteral("ui");
}
}

QString ModuleModel::key(const QString& repo, const QString& name)
{
    return repo + kSep + name;
}

ModuleModel::ModuleModel(QObject* parent) : QAbstractListModel(parent) {}

// ── QAbstractListModel ─────────────────────────────────────────────────────

int ModuleModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant ModuleModel::data(const QModelIndex& index, int role) const
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
    case ColorRole:            return r.color;
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
    case ActionRole: {
        if (m_installRegistry) {
            if (m_installRegistry->isInFlight(r.name))
                return QStringLiteral("installing");
            if (m_installRegistry->stage(r.name) == InstallStage::Installed)
                return QStringLiteral("installed");
        }
        return r.action;
    }
    case ToVersionRole:        return r.toVersion;
    case IsTopLevelRole:       return r.isTopLevel;
    case ResolverErrorRole:    return r.resolverError;
    case InstallStageRole:
        return m_installRegistry ? m_installRegistry->stage(r.name)
                            : static_cast<int>(InstallStage::None);
    case InstallErrorRole:
        return m_installRegistry ? m_installRegistry->error(r.name) : QString();
    case IsLoadedRole:         return r.isLoaded;
    case IsLoadingRole:        return r.isLoading;
    case IsMainUiRole:         return r.isMainUi;
    case IconPathRole:         return r.iconPath.isEmpty() ? r.iconUrl : r.iconPath;
    case HasMissingDepsRole:   return !r.missingDeps.isEmpty();
    case CpuRole:              return r.cpu;
    case MemoryRole:           return r.memory;
    case IsUiPluginRecordRole: return r.isUiPluginRecord;
    case IsCoreModuleRecordRole: return r.isCoreModuleRecord;
    }
    return {};
}

QHash<int, QByteArray> ModuleModel::roleNames() const
{
    return {
        {NameRole,             "name"},
        {RepositoryUrlRole,    "repositoryUrl"},
        {DisplayNameRole,      "displayName"},
        {DescriptionRole,      "description"},
        {CategoryRole,         "category"},
        {TypeRole,             "type"},
        {ColorRole,            "color"},
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
        {IsLoadedRole,         "isLoaded"},
        {IsLoadingRole,        "isLoading"},
        {IsMainUiRole,         "isMainUi"},
        {IconPathRole,         "iconPath"},
        {HasMissingDepsRole,   "hasMissingDeps"},
        {CpuRole,              "cpu"},
        {MemoryRole,           "memory"},
        {IsUiPluginRecordRole, "isUiPluginRecord"},
        {IsCoreModuleRecordRole, "isCoreModuleRecord"},
    };
}

QStringList ModuleModel::categories() const
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

void ModuleModel::recomputeInstallStatus(Row& r)
{
    if (r.installedVersion.isEmpty()) {
        r.installStatus = InstallStatus::NotInstalled;
        return;
    }
    if (!r.missingDeps.isEmpty()) {
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

void ModuleModel::recomputeVersionDerivedFields(Row& r)
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

void ModuleModel::replaceCatalog(const QVariantList& catalogRows)
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
        const Row& r = m_rows[i];
        // Rows seeded from on-disk scans (UI plugins / core modules) use keys
        // that often don't match catalog (repo, name) pairs. Never drop them
        // here — sidebar and settings tabs filter on these flags.
        if (r.isUiPluginRecord || r.isCoreModuleRecord) continue;
        const QString k = key(r.repositoryUrl, r.name);
        if (!incoming.contains(k)) toRemove.append(i);
    }
    for (int i = toRemove.size() - 1; i >= 0; --i) {
        const int idx = toRemove[i];
        beginRemoveRows({}, idx, idx);
        m_rows.removeAt(idx);
        endRemoveRows();
    }
    m_indexByKey.clear();
    m_indicesByName.clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        m_indexByKey.insert(key(m_rows[i].repositoryUrl, m_rows[i].name), i);
        m_indicesByName.insert(m_rows[i].name, i);
    }

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
            r.color          = row.value("color").toString();
            r.iconUrl        = row.value("iconUrl").toString();
            r.versions       = row.value("versions").toList();
            recomputeVersionDerivedFields(r);
            m_rows.append(std::move(r));
            m_indexByKey.insert(k, idx);
            m_indicesByName.insert(name, idx);
            endInsertRows();
        } else {
            // Update catalog fields on existing row, preserve everything else.
            const int idx = it.value();
            Row& r = m_rows[idx];
            r.displayName = row.value("displayName").toString();
            r.description = row.value("description").toString();
            r.category    = row.value("category").toString();
            r.type        = row.value("type").toString();
            r.color       = row.value("color").toString();
            r.iconUrl     = row.value("iconUrl").toString();
            r.versions    = row.value("versions").toList();
            recomputeVersionDerivedFields(r);
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, {
                DisplayNameRole, DescriptionRole, CategoryRole, TypeRole,
                ColorRole, IconUrlRole, VersionsRole, LatestVersionRole,
                HasUpdateRole, DependenciesRole, InstallStatusRole
            });
        }
    }
    emit categoriesChanged();
}

// ── Mutation: on-disk state ────────────────────────────────────────────────

void ModuleModel::markInstalled(const QString& name,
                              const QString& installedVersion,
                              const QString& installedHash)
{
    bool anyChanged = false;
    for (int idx : m_indicesByName.values(name)) {
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
    if (!anyChanged || m_inBulkInstalledUpdate) return;

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

void ModuleModel::replaceInstalledSet(const QHash<QString, QString>& versionByName,
                                    const QHash<QString, QString>& hashByName)
{
    const bool wasBulk = m_inBulkInstalledUpdate;
    if (!wasBulk) beginBulkInstalledUpdate();

    for (int idx = 0; idx < m_rows.size(); ++idx) {
        Row& r = m_rows[idx];
        const auto it = versionByName.find(r.name);
        const bool isInstalled = (it != versionByName.end());
        const QString newVersion = isInstalled ? it.value() : QString();
        const QString newHash    = isInstalled ? hashByName.value(r.name) : QString();
        if (r.installedVersion == newVersion && r.installedHash == newHash) continue;
        r.installedVersion = newVersion;
        r.installedHash    = newHash;
        recomputeInstallStatus(r);
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {InstalledVersionRole, HasUpdateRole,
                                  IsInstalledRole, InstallStatusRole});
    }

    if (!wasBulk) endBulkInstalledUpdate();
}

void ModuleModel::beginBulkInstalledUpdate()
{
    m_inBulkInstalledUpdate = true;
}

void ModuleModel::endBulkInstalledUpdate()
{
    if (!m_inBulkInstalledUpdate) return;
    m_inBulkInstalledUpdate = false;

    for (int idx = 0; idx < m_rows.size(); ++idx) {
        Row& r = m_rows[idx];
        const InstallStatus::Value prev = r.installStatus;
        recomputeInstallStatus(r);
        if (r.installStatus != prev) {
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, {InstallStatusRole, IsInstalledRole});
        }
    }
}

void ModuleModel::setInstallType(const QString& name, const QString& installType)
{
    for (int idx : m_indicesByName.values(name)) {
        Row& r = m_rows[idx];
        if (r.installType == installType) continue;
        r.installType = installType;
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {InstallTypeRole});
    }
}

void ModuleModel::setDisplayName(const QString& name, const QString& displayName)
{
    for (int idx : m_indicesByName.values(name)) {
        Row& r = m_rows[idx];
        if (r.displayName == displayName) continue;
        r.displayName = displayName;
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {DisplayNameRole});
    }
}

void ModuleModel::setIconUrl(const QString& name, const QString& iconUrl)
{
    for (int idx : m_indicesByName.values(name)) {
        Row& r = m_rows[idx];
        if (r.iconUrl == iconUrl) continue;
        r.iconUrl = iconUrl;
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {IconUrlRole});
    }
}

void ModuleModel::setMissingDeps(const QString& name, const QStringList& missing)
{
    for (int idx : m_indicesByName.values(name)) {
        Row& r = m_rows[idx];
        if (r.missingDeps == missing) continue;
        r.missingDeps = missing;
        recomputeInstallStatus(r);
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {MissingDepsRole, IsInstalledRole, InstallStatusRole});
    }
}

// ── Wiring: live install state ────────────────────────────────────────────

void ModuleModel::setInstallRegistry(InstallRegistry* installRegistry)
{
    if (m_installRegistry == installRegistry) return;
    if (m_installRegistry) m_installRegistry->disconnect(this);
    m_installRegistry = installRegistry;
    if (!m_installRegistry) return;

    auto refresh = [this](const QString& name) {
        const QList<int> roles{InstallStageRole, InstallErrorRole, ActionRole};
        for (int idx : m_indicesByName.values(name)) {
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, roles);
        }
    };
    connect(m_installRegistry, &InstallRegistry::stageChanged, this,
            [refresh](const QString& name, InstallStage::Value) { refresh(name); });
    connect(m_installRegistry, &InstallRegistry::errorChanged, this,
            [refresh](const QString& name, const QString&) { refresh(name); });
}

// ── Mutation: resolver overlay ─────────────────────────────────────────────

void ModuleModel::setResolverOverlay(const QList<ResolverRow>& rows)
{
    clearResolverOverlay();
    if (m_installRegistry) {
        for (const ResolverRow& src : rows) {
            if (!src.name.isEmpty())
                m_installRegistry->clear(src.name);
        }
    }
    for (const ResolverRow& src : rows) {
        const int idx = rowOf(src.name, src.repositoryUrl);
        if (idx < 0) continue;
        Row& r = m_rows[idx];
        r.action        = src.action;
        r.toVersion     = src.toVersion;
        r.isTopLevel    = src.isTopLevel;
        r.resolverError = src.resolverError;
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi,
            {ActionRole, ToVersionRole, IsTopLevelRole, ResolverErrorRole});
    }
}

void ModuleModel::clearResolverOverlay()
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

int ModuleModel::rowOf(const QString& name, const QString& repositoryUrl) const
{
    if (!repositoryUrl.isEmpty()) {
        const auto it = m_indexByKey.find(key(repositoryUrl, name));
        return it == m_indexByKey.end() ? -1 : it.value();
    }
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].name == name) return i;
    return -1;
}

QVariantMap ModuleModel::rowData(int row) const
{
    if (row < 0 || row >= m_rows.size()) return {};
    const QModelIndex mi = index(row);
    QVariantMap m;
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        m.insert(QString::fromUtf8(it.value()), data(mi, it.key()));
    return m;
}

QVariantMap ModuleModel::rowDataByName(const QString& name,
                                     const QString& repositoryUrl) const
{
    return rowData(rowOf(name, repositoryUrl));
}

void ModuleModel::seedInstalledOnly(const QString& name,
                                    const QString& type,
                                    const QVariantMap& fields)
{
    if (name.isEmpty() || type.isEmpty()) return;

    int idx = rowOf(name);
    if (idx >= 0) {
        Row& r = m_rows[idx];
        if (!r.type.isEmpty() && r.type != type) {
            qWarning() << "ModuleModel::seedInstalledOnly: type mismatch for"
                       << name << "(" << r.type << "vs" << type << ")";
            return;
        }
        const auto merge = [&](const QString& key, auto setter) {
            const QVariant v = fields.value(key);
            if (!v.isValid() || v.isNull()) return;
            setter(v);
        };
        merge(QStringLiteral("displayName"), [&](const QVariant& v) {
            if (r.displayName.isEmpty()) r.displayName = v.toString();
        });
        merge(QStringLiteral("description"), [&](const QVariant& v) {
            if (r.description.isEmpty()) r.description = v.toString();
        });
        merge(QStringLiteral("repositoryUrl"), [&](const QVariant& v) {
            if (r.repositoryUrl.isEmpty()) r.repositoryUrl = v.toString();
        });
        merge(QStringLiteral("version"), [&](const QVariant& v) {
            if (r.installedVersion.isEmpty()) r.installedVersion = v.toString();
        });
        merge(QStringLiteral("installType"), [&](const QVariant& v) {
            r.installType = v.toString();
        });
        merge(QStringLiteral("iconPath"), [&](const QVariant& v) {
            r.iconPath = v.toString();
            if (r.iconUrl.isEmpty()) r.iconUrl = v.toString();
        });
        merge(QStringLiteral("iconUrl"), [&](const QVariant& v) {
            if (r.iconUrl.isEmpty()) r.iconUrl = v.toString();
            if (r.iconPath.isEmpty()) r.iconPath = v.toString();
        });
        if (fields.contains(QStringLiteral("isMainUi")))
            r.isMainUi = fields.value(QStringLiteral("isMainUi")).toBool();
        if (isUiPluginType(type)) r.isUiPluginRecord = true;
        if (type == QStringLiteral("core")) r.isCoreModuleRecord = true;
        if (!r.type.isEmpty()) {
            recomputeInstallStatus(r);
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, {
                DisplayNameRole, DescriptionRole, RepositoryUrlRole,
                InstalledVersionRole, InstallTypeRole, IconUrlRole, IconPathRole,
                IsMainUiRole, IsInstalledRole, InstallStatusRole,
                IsUiPluginRecordRole, IsCoreModuleRecordRole
            });
        } else {
            r.type = type;
            if (isUiPluginType(type)) r.isUiPluginRecord = true;
            if (type == QStringLiteral("core")) r.isCoreModuleRecord = true;
            if (r.installedVersion.isEmpty() && !fields.value(QStringLiteral("version")).toString().isEmpty())
                r.installedVersion = fields.value(QStringLiteral("version")).toString();
            else if (r.installedVersion.isEmpty())
                r.installedVersion = QStringLiteral("0");
            recomputeInstallStatus(r);
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, {
                TypeRole, DisplayNameRole, InstalledVersionRole, InstallTypeRole,
                IconUrlRole, IconPathRole, IsMainUiRole, IsInstalledRole,
                InstallStatusRole, IsUiPluginRecordRole, IsCoreModuleRecordRole
            });
        }
        return;
    }

    const int newIdx = m_rows.size();
    beginInsertRows({}, newIdx, newIdx);
    Row r;
    r.name = name;
    r.type = type;
    r.displayName = fields.value(QStringLiteral("displayName")).toString();
    r.description = fields.value(QStringLiteral("description")).toString();
    r.repositoryUrl = fields.value(QStringLiteral("repositoryUrl")).toString();
    r.installedVersion = fields.value(QStringLiteral("version")).toString();
    if (r.installedVersion.isEmpty()) r.installedVersion = QStringLiteral("0");
    r.installType = fields.value(QStringLiteral("installType")).toString();
    r.iconPath = fields.value(QStringLiteral("iconPath")).toString();
    r.iconUrl = fields.value(QStringLiteral("iconUrl")).toString();
    if (r.iconUrl.isEmpty()) r.iconUrl = r.iconPath;
    if (r.iconPath.isEmpty()) r.iconPath = r.iconUrl;
    r.isMainUi = fields.value(QStringLiteral("isMainUi")).toBool();
    r.isUiPluginRecord = isUiPluginType(type);
    r.isCoreModuleRecord = (type == QStringLiteral("core"));
    recomputeInstallStatus(r);
    m_rows.append(std::move(r));
    m_indexByKey.insert(key(r.repositoryUrl, name), newIdx);
    m_indicesByName.insert(name, newIdx);
    endInsertRows();
    emit categoriesChanged();
}

void ModuleModel::setRoleByName(const QString& name, int role, const QVariant& value)
{
    const QList<int> indices = m_indicesByName.values(name);
    if (indices.isEmpty()) return;

    QList<int> changedRoles;
    for (int idx : indices) {
        if (idx < 0 || idx >= m_rows.size()) continue;
        Row& r = m_rows[idx];
        bool changed = false;
        switch (role) {
        case IsLoadedRole:
            if (r.isLoaded != value.toBool()) { r.isLoaded = value.toBool(); changed = true; }
            break;
        case IsLoadingRole:
            if (r.isLoading != value.toBool()) { r.isLoading = value.toBool(); changed = true; }
            break;
        case IsMainUiRole:
            if (r.isMainUi != value.toBool()) { r.isMainUi = value.toBool(); changed = true; }
            break;
        case IconPathRole:
        case IconUrlRole: {
            const QString path = value.toString();
            if (r.iconPath != path) { r.iconPath = path; changed = true; }
            if (r.iconUrl != path) { r.iconUrl = path; changed = true; }
            if (changed) changedRoles << IconPathRole << IconUrlRole;
            break;
        }
        case CpuRole:
            if (r.cpu != value.toString()) { r.cpu = value.toString(); changed = true; }
            break;
        case MemoryRole:
            if (r.memory != value.toString()) { r.memory = value.toString(); changed = true; }
            break;
        case InstallTypeRole:
            if (r.installType != value.toString()) { r.installType = value.toString(); changed = true; }
            break;
        case MissingDepsRole: {
            const QStringList missing = value.toStringList();
            if (r.missingDeps != missing) {
                r.missingDeps = missing;
                recomputeInstallStatus(r);
                changed = true;
                changedRoles << MissingDepsRole << HasMissingDepsRole
                             << IsInstalledRole << InstallStatusRole;
            }
            break;
        }
        default:
            break;
        }
        if (changed && changedRoles.isEmpty())
            changedRoles << role;
        if (!changedRoles.isEmpty()) {
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, changedRoles);
            changedRoles.clear();
        }
    }
}
