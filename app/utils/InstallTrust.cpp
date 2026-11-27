#include "InstallTrust.h"

#include "BuiltInApps.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>

#include <utility>

namespace logos::trust {

Package asReplacement(Package package, const QVariantMap& record, const QStringList& bundled,
                      const LogosKeys& logos)
{
    // Names compare as a case-insensitive file system does, where Wallet_UI
    // lands on wallet_ui.
    if (builtin::isBuiltIn(package.name) || isReservedName(package.name, identityNames())) {
        package.status = Status::Invalid;
        package.builtIn = true;
        return package;
    }
    if (isReservedName(package.name, bundled) && !logos.dids.contains(package.did)) {
        package.status = Status::Invalid;
        package.reserved = true;
        return package;
    }
    const QString installType = record.value(QStringLiteral("installType")).toString();
    // Basecamp's own copy, Logos Storage's the one any publisher may replace,
    // is Logos's.
    if (installType == QLatin1String("embedded")) {
        if (!logos.dids.isEmpty() && !logos.dids.contains(package.did))
            package.replacesSignedBy = logos.dids.first();
        return package;
    }
    if (installType != QLatin1String("user"))
        return package;
    const QString had = record.value(QStringLiteral("signerDid")).toString();
    if (!isCanonicalDid(had)) {
        // Nobody Basecamp can name signed the copy on disk: only Logos may
        // take its place unasked.
        if (!package.did.isEmpty() && !logos.dids.contains(package.did))
            package.replacesUnverified = true;
        return package;
    }
    const bool logosBefore = logos.dids.contains(had) || logos.revoked.contains(had);
    if (had != package.did && !(logosBefore && logos.dids.contains(package.did)))
        package.replacesSignedBy = had;
    return package;
}

QStringList packageModuleCallers()
{
    return defaultapps::packageModuleCallers();
}

Package asInstall(Package package, const QString& type, const QVariantMap& record,
                  const QVariantMap& backendRecord, const QStringList& bundled, const LogosKeys& logos)
{
    package = asReplacement(std::move(package), record, bundled, logos);
    // An app's backend runs as `<name>_backend`. A package installed under
    // that name would keep it from running, and pass for it.
    if (type == QLatin1String("ui_qml") && !backendRecord.isEmpty()) {
        package.status = Status::Invalid;
        package.backendTaken = true;
    }
    return package;
}

QStringList identityNames()
{
    return {QStringLiteral("core"), QStringLiteral("core_service"), QStringLiteral("main_ui"),
            QStringLiteral("logos_link")};
}

QStringList updatableBundledModules()
{
    return {QStringLiteral("storage_module")};
}

QStringList reservedNames(const QStringList& modules, const QStringList& apps)
{
    QStringList names;
    for (const QString& module : modules)
        if (!updatableBundledModules().contains(module, Qt::CaseInsensitive)) names << module;
    names << packageModuleCallers() << QStringLiteral("intents_module");
    for (const QString& app : apps)
        names << app << app + QStringLiteral("_backend");
    return names;
}

QStringList reservedFor(const QStringList& bundled, const QVariantList& installed,
                        const QVariantList& plan)
{
    QStringList names = bundled;
    for (const QVariantList* packages : {&installed, &plan}) {
        for (const QVariant& v : *packages) {
            const QVariantMap package = v.toMap();
            if (package.value(QStringLiteral("type")).toString() == QLatin1String("ui_qml"))
                names << package.value(QStringLiteral("name")).toString() + QStringLiteral("_backend");
        }
    }
    return names;
}

bool isReservedName(const QString& name, const QStringList& reserved)
{
    return reserved.contains(name, Qt::CaseInsensitive);
}

QList<Package> judgePlan(const QString& name, const QVariantList& resolved,
                         const std::function<bool(const QVariantMap&)>& onDisk,
                         const std::function<QVariantList(const QVariantMap&)>& versionsOf,
                         const std::function<QVariantMap(const QVariantMap&)>& installedOf,
                         const QStringList& trusted, const QStringList& bundled,
                         const LogosKeys& logos)
{
    // A planned row: what is on disk already, judged by that copy, or what
    // the install would fetch, by the catalogue. `name` is always fetched: a
    // reinstall replaces the copy on disk.
    const auto judge = [&](const QVariantMap& row) {
        const QString rowName = row.value(QStringLiteral("name")).toString();
        const QString version = row.value(QStringLiteral("version")).toString();
        if (rowName == name || !onDisk(row))
            return asInstall(fromCatalog(rowName, version, row.value(QStringLiteral("rootHash")).toString(),
                                         versionsOf(row), trusted),
                             row.value(QStringLiteral("type")).toString(), installedOf(row),
                             installedOf({{QStringLiteral("name"), rowName + QStringLiteral("_backend")}}),
                             bundled, logos);
        return fromInstalled(rowName, version, installedOf(row), trusted);
    };

    QList<Package> packages;
    QStringList judged;
    if (resolved.isEmpty())
        packages.append({name, {}, Status::Unknown});
    for (const QVariant& v : resolved) {
        const QVariantMap row = v.toMap();
        const QString rowName = row.value(QStringLiteral("name")).toString();
        if (rowName.isEmpty() || row.contains(QStringLiteral("error"))) {
            packages.append({rowName.isEmpty() ? name : rowName,
                             row.value(QStringLiteral("version")).toString(), Status::Unknown});
            continue;
        }
        judged << rowName;
        packages.append(judge(row));
    }
    // The optional packages offered and not chosen, as the offer would install them.
    for (const QVariant& v : resolved) {
        for (const QVariant& o : v.toMap().value(QStringLiteral("optionalDependencies")).toList()) {
            const QVariantMap offer = o.toMap();
            const QString offerName = offer.value(QStringLiteral("name")).toString();
            if (offerName.isEmpty() || offer.contains(QStringLiteral("error")) || judged.contains(offerName))
                continue;
            judged << offerName;
            const QVariantMap request = offer.value(QStringLiteral("request")).toMap();
            Package package = judge({
                {QStringLiteral("name"), offerName},
                {QStringLiteral("version"), offer.value(QStringLiteral("version"))},
                {QStringLiteral("rootHash"), offer.value(QStringLiteral("rootHash"))},
                {QStringLiteral("repositoryUrl"),
                 offer.value(QStringLiteral("repositoryUrl"), request.value(QStringLiteral("repositoryUrl")))}});
            package.offered = true;
            packages.append(package);
        }
    }
    return packages;
}

QString localPackagePath(const QString& path, bool fromPackageManager)
{
    if (!fromPackageManager || path.size() < 2)
        return {};
    // A network share: `\\server\…`, `//server/…`, `\\?\UNC\…`, mixed as Windows allows.
    const auto separator = [](QChar c) { return c == QLatin1Char('/') || c == QLatin1Char('\\'); };
    if (separator(path.at(0)) && separator(path.at(1)))
        return {};
    if (!QDir::isAbsolutePath(path) || !path.endsWith(QLatin1String(".lgx"), Qt::CaseInsensitive))
        return {};
    return path;
}

QVariantList approvedPlan(const QString& name, const QVariantList& resolved,
                          const QList<Package>& judged)
{
    QHash<QString, Package> byName;
    for (const Package& p : judged)
        if (!p.offered && !byName.contains(p.name))
            byName.insert(p.name, p);
    QVariantList plan;
    for (const QVariant& v : resolved) {
        const QVariantMap row = v.toMap();
        const QString rowName = row.value(QStringLiteral("name")).toString();
        if (rowName.isEmpty() || row.contains(QStringLiteral("error")))
            continue;
        const Package package = byName.value(rowName);
        if (package.installed && rowName != name)
            continue;
        QVariantMap request{{QStringLiteral("name"), rowName},
                            {QStringLiteral("version"), row.value(QStringLiteral("version"))},
                            {QStringLiteral("rootHash"), row.value(QStringLiteral("rootHash"))},
                            {QStringLiteral("repositoryUrl"), row.value(QStringLiteral("repositoryUrl"))}};
        if (!package.installed
            && (package.status == Status::Trusted || package.status == Status::Untrusted))
            request.insert(QStringLiteral("signer"), package.did);
        // Unsigned, the file's digest is what pins its manifest: rootHash
        // covers only the payload.
        if (!package.installed && package.status == Status::Unsigned && !package.sha256.isEmpty())
            request.insert(QStringLiteral("sha256"), package.sha256);
        if (row.value(QStringLiteral("optional")).toBool())
            request.insert(QStringLiteral("optional"), true);
        plan << request;
    }
    return plan;
}

bool requestsLocalFile(const QVariantMap& params)
{
    return !params.value(QStringLiteral("localPath")).toString().isEmpty();
}

QVariantMap fileIdentity(const QString& name, const QString& version,
                         const QVariantMap& verified, const QVariantMap& inspected,
                         const QString& sha256)
{
    const bool verifiedSigned = verified.value(QStringLiteral("isSigned")).toBool()
                                && verified.value(QStringLiteral("signatureValid")).toBool()
                                && verified.value(QStringLiteral("packageValid")).toBool();
    const QString signer = verifiedSigned ? verified.value(QStringLiteral("signerDid")).toString()
                                          : QString();
    const QString inspectedSigner =
        inspected.value(QStringLiteral("signatureStatus")).toString() == QLatin1String("signed")
            ? inspected.value(QStringLiteral("signerDid")).toString() : QString();
    const QString rootHash = inspected.value(QStringLiteral("rootHash")).toString();
    if (rootHash.isEmpty() || sha256.isEmpty() || inspected.contains(QStringLiteral("error"))
        || inspected.value(QStringLiteral("name")).toString() != name
        || inspected.value(QStringLiteral("version")).toString() != version
        || inspectedSigner != signer)
        return {};
    return {{QStringLiteral("rootHash"), rootHash}, {QStringLiteral("signer"), signer},
            {QStringLiteral("sha256"), sha256}};
}

QString sha256Of(const QString& path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace logos::trust
