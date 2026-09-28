#include "AppsModel.h"

#include "InstallRegistry.h"

#include <logos/semver.hpp>

#include <QSet>

namespace {
constexpr QChar kSep = QLatin1Char('\n');
// The bundler refuses to emit a 0.4.0 package whose icon is not a validated
// 256x256 assets/icon.png, so the version is a guarantee that the artwork is
// safe to render edge-to-edge. Below 0.4.0 the icon is whatever the author
// shipped — typically a 24-28px glyph that must stay inset.
//
// Mirrors Manifest::requiresIconContract() in logos-package, the authority
// for this rule; keep the two in step.
bool manifestSupportsFullBleedIconImpl(const QString& manifestVersion)
{
    const QStringList parts = manifestVersion.split(QLatin1Char('.'));
    if (parts.size() < 2) return false;
    bool okMajor = false, okMinor = false;
    const int major = parts.at(0).toInt(&okMajor);
    const int minor = parts.at(1).toInt(&okMinor);
    if (!okMajor || !okMinor) return false;
    if (major > 0) return true;
    return major == 0 && minor >= 4;
}

QString catalogIconUrl(const QVariantMap& row)
{
    const QString raw = row.value("icon").toString();
    return raw.contains(QLatin1String("://")) ? raw : QString();
}

// ── Availability on this host ───────────────────────────────────────────────
//
// DUPLICATE of logos-package-manager-ui's rule (src/PackageRowBuilder.cpp's
// splitVariant / classifyNotAvailable, and RowActionResolver.h's
// hasInstallableVersion). Same inputs, and they must give the same answer —
// when they drift, the Package Manager and the App Manager show a different
// set of packages for one catalog, which surfaces as a user report rather
// than a failing test. Change one, change the other.
//
// The honest shared homes were a header in logos-package or a
// `variantAvailable` stamp in package_downloader's getCatalog() beside the
// `sourceAvailable` it already writes; both are cross-repo. If either lands,
// delete this block.

// Split "<os>-<arch>[-<flavor>]" into (base, flavor). A dev-flavor host
// accepts ONLY "-dev" variants — PackageManagerLib::platformVariantsToTry
// REPLACES the bare spelling rather than adding to it — so the flavor has to
// be separable to tell "wrong flavor" from "wrong machine".
std::pair<QString, QString> splitVariant(const QString& v)
{
    static const QSet<QString> kKnownFlavors = {
        QStringLiteral("dev"), QStringLiteral("portable")
    };
    const int lastDash = v.lastIndexOf(QLatin1Char('-'));
    if (lastDash <= 0) return {v, QString()};
    const QString trailing = v.mid(lastDash + 1);
    if (kKnownFlavors.contains(trailing)) return {v.left(lastDash), trailing};
    return {v, QString()};
}

// Why the offered variants miss this host's. Only called when they do.
AppsModel::NotAvailableReason classifyUnavailable(const QStringList& offered,
                                                  const QStringList& valid)
{
    if (offered.isEmpty()) return AppsModel::NoVariantsPublished;
    QSet<QString> hostBases;
    for (const QString& v : valid) hostBases.insert(splitVariant(v).first);
    for (const QString& v : offered) {
        if (hostBases.contains(splitVariant(v).first))
            return AppsModel::BuildFlavorMismatch;
    }
    return AppsModel::PlatformMismatch;
}

// True iff at least one of `versions` is one this host could install.
//
// Variant availability is decided by the NEWEST version's manifest (matching
// PMUI); the download-source check is per version, so one servable version
// keeps the package. Deliberately a property of the whole package: a
// per-version answer would hide one whose newest release is unreachable but
// whose older ones are not.
bool computeAvailability(const QVariantList& versions,
                         const QStringList& validVariants,
                         AppsModel::NotAvailableReason* outReason)
{
    auto fail = [outReason](AppsModel::NotAvailableReason r) {
        if (outReason) *outReason = r;
        return false;
    };
    if (versions.isEmpty()) return fail(AppsModel::NoVariantsPublished);

    const QVariantMap manifest =
        versions.first().toMap().value(QStringLiteral("manifest")).toMap();

    // The variants a package offers are the keys of its manifest's `main` map.
    QStringList offered;
    const QVariantMap mainMap = manifest.value(QStringLiteral("main")).toMap();
    for (auto it = mainMap.constBegin(); it != mainMap.constEnd(); ++it) {
        if (!it.key().isEmpty()) offered.append(it.key());
    }

    bool variantOk = false;
    for (const QString& v : offered) {
        if (validVariants.contains(v)) { variantOk = true; break; }
    }
    // A QML-only package can carry an empty `main` (no backend plugin); it
    // installs everywhere.
    if (!variantOk && offered.isEmpty()
        && manifest.value(QStringLiteral("type")).toString() == QLatin1String("ui_qml")) {
        variantOk = true;
    }
    if (!variantOk) return fail(classifyUnavailable(offered, validVariants));

    // `sourceAvailable` is stamped per version by package_downloader under its
    // download-source setting. A downloader predating it sends nothing, and
    // everything defaults to servable.
    for (const QVariant& v : versions) {
        if (v.toMap().value(QStringLiteral("sourceAvailable"), true).toBool()) {
            if (outReason) *outReason = AppsModel::Available;
            return true;
        }
    }
    // Every version refused by the download source. No variant reason applies:
    // the package does build for this host, the transport is the problem.
    return fail(AppsModel::Available);
}
}

bool AppsModel::supportsFullBleedIcon(const QString& manifestVersion)
{
    return manifestSupportsFullBleedIconImpl(manifestVersion);
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
    case SupportsFullBleedIconRole: return r.supportsFullBleedIcon;
    case VersionsRole:         return r.versions;
    case DependenciesRole:     return r.dependencies;
    case ProvidesRole:         return r.provides;
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
        if (InstallRegistry* reg = registryFor(r)) {
            if (reg->isInFlight(r.name))
                return QStringLiteral("installing");
            if (reg->stage(r.name) == InstallStage::Installed)
                return QStringLiteral("installed");
        }
        return r.action;
    }
    case ToVersionRole:        return r.toVersion;
    case IsTopLevelRole:       return r.isTopLevel;
    case ResolverErrorRole:    return r.resolverError;
    case InstallStageRole: {
        InstallRegistry* reg = registryFor(r);
        return reg ? reg->stage(r.name) : static_cast<int>(InstallStage::None);
    }
    case InstallErrorRole: {
        InstallRegistry* reg = registryFor(r);
        return reg ? reg->error(r.name) : QString();
    }
    case DownloadReceivedRole: {
        InstallRegistry* reg = registryFor(r);
        return QVariant::fromValue(reg ? reg->downloadReceived(r.name) : quint64(0));
    }
    case DownloadTotalRole: {
        InstallRegistry* reg = registryFor(r);
        return QVariant::fromValue(reg ? reg->downloadTotal(r.name) : quint64(0));
    }
    case PlanDownloadReceivedRole: {
        InstallRegistry* reg = registryFor(r);
        return QVariant::fromValue(reg ? reg->planDownloadReceived(r.name) : quint64(0));
    }
    case PlanDownloadTotalRole: {
        InstallRegistry* reg = registryFor(r);
        return QVariant::fromValue(reg ? reg->planDownloadTotal(r.name) : quint64(0));
    }
    case PlanInstallStageRole: {
        InstallRegistry* reg = registryFor(r);
        return reg ? reg->planStage(r.name) : static_cast<int>(InstallStage::None);
    }
    case HasInstallableVersionRole: return r.hasInstallableVersion;
    case NotAvailableReasonRole:    return r.notAvailableReason;
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
        {SupportsFullBleedIconRole, "supportsFullBleedIcon"},
        {VersionsRole,         "versions"},
        {DependenciesRole,     "dependencies"},
        {ProvidesRole,         "provides"},
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
        {DownloadReceivedRole,     "downloadReceived"},
        {DownloadTotalRole,        "downloadTotal"},
        {PlanDownloadReceivedRole, "planDownloadReceived"},
        {PlanDownloadTotalRole,    "planDownloadTotal"},
        {PlanInstallStageRole,     "planInstallStage"},
        {HasInstallableVersionRole, "hasInstallableVersion"},
        {NotAvailableReasonRole,    "notAvailableReason"},
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

// Three-way version compare. Returns -1 / 0 / +1.
//
// Delegates to the shared semver implementation in logos-package — the same
// code lgx, lgpm, lgpd and the package-manager UI use. It used to split on '.'
// and QString::toInt() each component (dropping the `ok` flag), so
// "0-rc1".toInt() silently yielded 0 and every pre-release compared EQUAL to
// its own release: an installed 1.0.0-rc.1 against an available 1.0.0 showed no
// Upgrade, and 1.0.0-rc.2 vs 1.0.0-rc.10 compared equal.
static int versionCmp(const QString& a, const QString& b)
{
    return logos::semver::compare(a.toStdString(), b.toStdString());
}

void AppsModel::recomputeInstallStatus(Row& r)
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

    InstallStatus::Value worstDep = InstallStatus::Installed;
    const auto isWorse = [](InstallStatus::Value candidate,
                            InstallStatus::Value current) {
        const auto rank = [](InstallStatus::Value v) {
            switch (v) {
            case InstallStatus::DifferentHash:      return 3;
            case InstallStatus::DowngradeAvailable: return 2;
            case InstallStatus::UpgradeAvailable:   return 1;
            default:                                return 0;
            }
        };
        return rank(candidate) > rank(current);
    };

    for (const QVariant& v : r.dependencies) {
        const QVariantMap dep = v.toMap();
        const QString depName = dep.value("name").toString();
        if (depName.isEmpty()) continue;
        const int depIdx = rowOf(depName, r.repositoryUrl);
        if (depIdx < 0) continue;   // dep not in this repo — see comment above
        const Row& depRow = m_rows[depIdx];
        if (depRow.versions.isEmpty()) continue;

        const QString depRelease = depRow.latestVersion;
        if (!depRelease.isEmpty() && !depRow.installedVersion.isEmpty()) {
            const int depCmp = versionCmp(depRow.installedVersion, depRelease);
            if (depCmp < 0) {
                if (isWorse(InstallStatus::UpgradeAvailable, worstDep))
                    worstDep = InstallStatus::UpgradeAvailable;
                continue;
            }
            if (depCmp > 0) {
                if (isWorse(InstallStatus::DowngradeAvailable, worstDep))
                    worstDep = InstallStatus::DowngradeAvailable;
                continue;
            }
        }

        // Same version (or no version to compare): a hash mismatch is
        // corruption, and only a reinstall repairs it.
        const QString expectedDepHash =
            depRow.versions.first().toMap().value("rootHash").toString();
        const QString installedDepHash = depRow.installedHash;
        if (expectedDepHash.isEmpty() || installedDepHash.isEmpty()) continue;
        if (expectedDepHash != installedDepHash
            && isWorse(InstallStatus::DifferentHash, worstDep)) {
            worstDep = InstallStatus::DifferentHash;
        }
    }

    r.installStatus = worstDep;
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

    // Intents the package advertises (names only)
    r.provides.clear();
    for (const QVariant& v : firstManifest.value("provides").toList()) {
        const QString intent = v.typeId() == QMetaType::QString
            ? v.toString()
            : v.toMap().value(QStringLiteral("intent")).toString();
        if (!intent.isEmpty() && !r.provides.contains(intent))
            r.provides.append(intent);
    }

    // Availability on this host. An empty valid-variant list means
    // package_manager has not answered yet: treat everything as installable
    // rather than blanking the grid until it does.
    NotAvailableReason reason = Available;
    r.hasInstallableVersion = m_validVariants.isEmpty()
        ? true
        : computeAvailability(r.versions, m_validVariants, &reason);
    r.notAvailableReason = static_cast<int>(reason);

    recomputeInstallStatus(r);
}

void AppsModel::setValidVariants(const QStringList& variants)
{
    if (variants == m_validVariants) return;
    m_validVariants = variants;
    emit hostVariantChanged();
    if (m_rows.isEmpty()) return;

    for (int i = 0; i < m_rows.size(); ++i) {
        Row& r = m_rows[i];
        const bool wasAvailable = r.hasInstallableVersion;
        const int  wasReason    = r.notAvailableReason;

        NotAvailableReason reason = Available;
        r.hasInstallableVersion = m_validVariants.isEmpty()
            ? true
            : computeAvailability(r.versions, m_validVariants, &reason);
        r.notAvailableReason = static_cast<int>(reason);

        if (r.hasInstallableVersion != wasAvailable || r.notAvailableReason != wasReason) {
            const QModelIndex mi = index(i);
            emit dataChanged(mi, mi, {HasInstallableVersionRole, NotAvailableReasonRole});
        }
    }
}

// ── Mutation: bulk replace from catalog ────────────────────────────────────

void AppsModel::replaceCatalog(const QVariantList& catalogRows)
{
    QSet<QString> incoming;
    QSet<QString> incomingNames;
    incoming.reserve(catalogRows.size());
    incomingNames.reserve(catalogRows.size());
    for (const QVariant& v : catalogRows) {
        const QVariantMap row = v.toMap();
        const QString name = row.value("name").toString();
        if (name.isEmpty()) continue;
        incoming.insert(key(row.value("repositoryUrl").toString(), name));
        incomingNames.insert(name);
    }

    QList<int> toRemove;
    for (int i = 0; i < m_rows.size(); ++i) {
        // Local rows (no repositoryUrl) survive a catalog replace UNLESS the
        // incoming catalog now provides that name — in which case the catalog
        // row is the source of truth and the local one is dropped.
        if (m_rows[i].repositoryUrl.isEmpty()) {
            if (incomingNames.contains(m_rows[i].name)) toRemove.append(i);
            continue;
        }
        const QString k = key(m_rows[i].repositoryUrl, m_rows[i].name);
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
            r.iconUrl        = catalogIconUrl(row);
            r.supportsFullBleedIcon = AppsModel::supportsFullBleedIcon(
                row.value("manifestVersion").toString());
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
            const QString ic = catalogIconUrl(row);
            if (!ic.isEmpty())
                r.iconUrl = ic;
            else if (r.installedVersion.isEmpty())
                r.iconUrl.clear();
            r.supportsFullBleedIcon = AppsModel::supportsFullBleedIcon(
                row.value("manifestVersion").toString());
            r.versions    = row.value("versions").toList();
            recomputeVersionDerivedFields(r);
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, {
                DisplayNameRole, DescriptionRole, CategoryRole, TypeRole,
                IconUrlRole, SupportsFullBleedIconRole,
                VersionsRole, LatestVersionRole,
                HasUpdateRole, DependenciesRole, InstallStatusRole
            });
        }
    }
    emit categoriesChanged();
}

// ── Mutation: local-only installed rows ────────────────────────────────────

void AppsModel::mergeLocalOnlyInstalled(const QVariantList& installedPackages)
{
    // Names still present on disk. Used to drop stale Local rows below.
    QSet<QString> freshNames;
    freshNames.reserve(installedPackages.size());
    for (const QVariant& v : installedPackages) {
        const QString name = v.toMap().value("name").toString();
        if (!name.isEmpty()) freshNames.insert(name);
    }

    // Prune Local rows whose module got uninstalled. Catalog rows (non-
    // empty repositoryUrl) are replaceCatalog's job — untouched here.
    QList<int> toRemove;
    bool categoriesTouched = false;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (!m_rows[i].repositoryUrl.isEmpty()) continue;
        if (freshNames.contains(m_rows[i].name)) continue;
        toRemove.append(i);
        if (!m_rows[i].category.isEmpty()) categoriesTouched = true;
    }
    for (int i = toRemove.size() - 1; i >= 0; --i) {
        const int idx = toRemove[i];
        beginRemoveRows({}, idx, idx);
        m_rows.removeAt(idx);
        endRemoveRows();
    }
    if (!toRemove.isEmpty()) {
        m_indexByKey.clear();
        m_indicesByName.clear();
        for (int i = 0; i < m_rows.size(); ++i) {
            m_indexByKey.insert(key(m_rows[i].repositoryUrl, m_rows[i].name), i);
            m_indicesByName.insert(m_rows[i].name, i);
        }
    }

    for (const QVariant& v : installedPackages) {
        const QVariantMap pkg = v.toMap();
        const QString name = pkg.value("name").toString();
        if (name.isEmpty()) continue;

        // Only surface USER-installed packages (under Application Support)
        // as Local. Embedded packages ship inside the app bundle and are
        // already represented via the built-in module list — showing them
        // here would double-list them and clutter the section.
        if (pkg.value("installType").toString() != QLatin1String("user")) continue;

        // Skip if any row for this name exists (catalog or previously merged
        // local). markInstalled/setInstallType/setIconUrl already reach the
        // existing row via m_indicesByName; nothing to do here.
        if (m_indicesByName.contains(name)) continue;

        const int idx = m_rows.size();
        beginInsertRows({}, idx, idx);
        Row r;
        r.name             = name;
        r.repositoryUrl    = QString();
        r.displayName      = pkg.value("displayName").toString();
        r.description      = pkg.value("description").toString();
        r.category         = pkg.value("category").toString();
        r.type             = pkg.value("type").toString();
        r.installedVersion = pkg.value("version").toString();
        r.installedHash    = pkg.value("hashes").toMap().value("root").toString();
        r.installType      = pkg.value("installType").toString();
        r.supportsFullBleedIcon = AppsModel::supportsFullBleedIcon(
            pkg.value("manifestVersion").toString());
        // versions{} + empty latestVersion → recomputeInstallStatus lands on
        // InstallStatus::Installed (installedVersion set + no release to
        // compare against). HasUpdate stays false. Local-only rows expose no
        // Install/Upgrade action.
        recomputeVersionDerivedFields(r);
        m_rows.append(std::move(r));
        m_indexByKey.insert(key(QString(), name), idx);
        m_indicesByName.insert(name, idx);
        endInsertRows();

        if (!m_rows.last().category.isEmpty()) categoriesTouched = true;
    }
    if (categoriesTouched) emit categoriesChanged();
}

// ── Mutation: on-disk state ────────────────────────────────────────────────

void AppsModel::markInstalled(const QString& name,
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

void AppsModel::replaceInstalledSet(const QHash<QString, QString>& versionByName,
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

void AppsModel::beginBulkInstalledUpdate()
{
    m_inBulkInstalledUpdate = true;
}

void AppsModel::endBulkInstalledUpdate()
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

void AppsModel::setInstallType(const QString& name, const QString& installType)
{
    for (int idx : m_indicesByName.values(name)) {
        Row& r = m_rows[idx];
        if (r.installType == installType) continue;
        r.installType = installType;
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, {InstallTypeRole});
    }
}

void AppsModel::setIconUrl(const QString& name,
                           const QString& iconUrl,
                           const QString& manifestVersion)
{
    const bool fullBleed = AppsModel::supportsFullBleedIcon(manifestVersion);
    for (int idx : m_indicesByName.values(name)) {
        Row& r = m_rows[idx];
        const bool iconChanged     = r.iconUrl != iconUrl;
        const bool fullBleedChanged = r.supportsFullBleedIcon != fullBleed;
        if (!iconChanged && !fullBleedChanged) continue;
        r.iconUrl = iconUrl;
        r.supportsFullBleedIcon = fullBleed;
        QList<int> roles;
        if (iconChanged)      roles.append(IconUrlRole);
        if (fullBleedChanged) roles.append(SupportsFullBleedIconRole);
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi, roles);
    }
}

void AppsModel::setMissingDeps(const QString& name, const QStringList& missing)
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

// The install registry is keyed by package NAME, but a name is not unique
// across repositories — two repos can publish the same package, and this
// model renders a row for each. Only the row whose repository the
// operation targets may show its in-flight state; otherwise installing
// from one repo animates the other repo's copy of the row too.
//
// Returns nullptr for a row the operation does not belong to, so callers
// fall through to the same defaults they use when no registry is set.
InstallRegistry* AppsModel::registryFor(const Row& r) const
{
    if (!m_installRegistry) return nullptr;
    return m_installRegistry->belongsTo(r.name, r.repositoryUrl)
               ? m_installRegistry : nullptr;
}

void AppsModel::setInstallRegistry(InstallRegistry* installRegistry)
{
    if (m_installRegistry == installRegistry) return;
    if (m_installRegistry) m_installRegistry->disconnect(this);
    m_installRegistry = installRegistry;
    if (!m_installRegistry) return;

    auto refresh = [this](const QString& name) {
        const QList<int> roles{InstallStageRole, InstallErrorRole, ActionRole,
                               PlanInstallStageRole};
        for (int idx : m_indicesByName.values(name)) {
            const QModelIndex mi = index(idx);
            emit dataChanged(mi, mi, roles);
        }
    };
    connect(m_installRegistry, &InstallRegistry::stageChanged, this,
            [refresh](const QString& name, InstallStage::Value) { refresh(name); });
    connect(m_installRegistry, &InstallRegistry::errorChanged, this,
            [refresh](const QString& name, const QString&) { refresh(name); });
    // Narrower role set than `refresh`: this fires several times a second
    // per download, and re-evaluating ActionRole with it would churn every
    // binding on the row for no reason.
    connect(m_installRegistry, &InstallRegistry::planStageChanged, this,
            [this](const QString& topLevel) {
                const QList<int> roles{PlanInstallStageRole};
                for (int idx : m_indicesByName.values(topLevel)) {
                    const QModelIndex mi = index(idx);
                    emit dataChanged(mi, mi, roles);
                }
            });
    connect(m_installRegistry, &InstallRegistry::downloadProgressChanged, this,
            [this](const QString& name) {
                const QList<int> roles{DownloadReceivedRole, DownloadTotalRole,
                                       PlanDownloadReceivedRole, PlanDownloadTotalRole,
                                       PlanInstallStageRole};
                for (int idx : m_indicesByName.values(name)) {
                    const QModelIndex mi = index(idx);
                    emit dataChanged(mi, mi, roles);
                }
            });
}

// ── Mutation: resolver overlay ─────────────────────────────────────────────

void AppsModel::setResolverOverlay(const QList<ResolverRow>& rows)
{
    clearResolverOverlay();
    // A package can appear in the overlay more than once — as a user-pinned
    // top-level entry AND as a transitive dependency of another pinned package
    // (resolved to the newest catalog version). Both map to the SAME row via
    // rowOf(name, repo), so without a guard the transitive copy overwrites the
    // pin and the version dropdown snaps back to the newest release. An explicit
    // top-level entry is authoritative: once one has written a row, a later
    // non-top-level duplicate of the same package must not clobber it.
    // (Defensive — the resolver also no longer emits that duplicate.)
    QSet<int> pinnedRows;
    for (const ResolverRow& src : rows) {
        const int idx = rowOf(src.name, src.repositoryUrl);
        if (idx < 0) continue;
        if (!src.isTopLevel && pinnedRows.contains(idx)) continue;
        Row& r = m_rows[idx];
        r.action        = src.action;
        r.toVersion     = src.toVersion;
        r.isTopLevel    = src.isTopLevel;
        r.resolverError = src.resolverError;
        if (src.isTopLevel) pinnedRows.insert(idx);
        const QModelIndex mi = index(idx);
        emit dataChanged(mi, mi,
            {ActionRole, ToVersionRole, IsTopLevelRole, ResolverErrorRole});
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
