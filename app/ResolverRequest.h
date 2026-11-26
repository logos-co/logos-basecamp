#pragma once

#include <QJsonArray>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace logos {

// What an installer app chose with an install, from a confirm request's
// params: the version of each package the plan brings (`versions`, name to
// version) and which optional packages to take (`optional`, name to bool).
// The resolver takes them as pins, so the plan Basecamp shows, and the person
// approves, is the one picked. Malformed entries are dropped.
struct InstallChoices {
    QVariantMap pins;          // required: name -> version, the subject's own included
    QVariantMap selection;     // optional: name -> taken
    QVariantMap optionalPins;  // optional: name -> version
};

inline InstallChoices installChoicesFrom(const QVariantMap& params, const QString& subject,
                                         const QString& subjectVersion)
{
    constexpr int kMaxEntries = 256;
    constexpr int kMaxVersionLength = 64;
    InstallChoices c;
    c.pins.insert(subject, subjectVersion);
    const QVariantMap optional = params.value(QStringLiteral("optional")).toMap();
    int seen = 0;
    for (auto it = optional.cbegin(); it != optional.cend() && seen < kMaxEntries; ++it, ++seen) {
        if (it.key().isEmpty() || it.key() == subject || it.value().typeId() != QMetaType::Bool)
            continue;
        c.selection.insert(it.key(), it.value().toBool());
    }
    const QVariantMap versions = params.value(QStringLiteral("versions")).toMap();
    seen = 0;
    for (auto it = versions.cbegin(); it != versions.cend() && seen < kMaxEntries; ++it, ++seen) {
        const QString version = it.value().typeId() == QMetaType::QString ? it.value().toString() : QString();
        if (it.key().isEmpty() || it.key() == subject || version.isEmpty()
            || version.size() > kMaxVersionLength)
            continue;
        (c.selection.contains(it.key()) ? c.optionalPins : c.pins).insert(it.key(), version);
    }
    return c;
}

// The requester's choices with what the person then changed in the dialog
// over them: which optional packages come, and at what version. The versions
// of what the plan needs stay the requester's.
inline InstallChoices withDialogPicks(InstallChoices c, const QVariantMap& selection,
                                      const QVariantMap& optionalPins)
{
    for (auto it = selection.cbegin(); it != selection.cend(); ++it) c.selection.insert(it.key(), it.value());
    for (auto it = optionalPins.cbegin(); it != optionalPins.cend(); ++it)
        c.optionalPins.insert(it.key(), it.value());
    return c;
}

inline QVariantList selectedOptionalRequests(const QVariantList& offers,
                                            const QStringList& names,
                                            const QVariantMap& versionPins = {})
{
    QVariantList requests;
    for (const QVariant& v : offers) {
        const QVariantMap offer = v.toMap();
        const QString name = offer.value("name").toString();
        if (!names.contains(name) || offer.contains("error")) continue;
        QVariantMap request = offer.value("request").toMap();
        const QString picked = versionPins.value(name).toString();
        if (!picked.isEmpty() && picked != offer.value("version").toString()) {
            bool found = false;
            for (const QVariant& candidate : offer.value("versions").toList()) {
                const QVariantMap version = candidate.toMap();
                if (!version.value("sourceAvailable", true).toBool()
                    || version.value("manifest").toMap().value("version").toString() != picked)
                    continue;
                request.insert("version", picked);
                request.insert("rootHash", version.value("rootHash"));
                found = true;
                break;
            }
            if (!found) continue;
        }
        requests.append(request);
    }
    return requests;
}

// True when `request` would reinstall the optional release already installed.
inline bool keepsInstalledRelease(const QVariantMap& offer, const QVariantMap& request)
{
    if (!offer.contains("installedVersion")
        || request.value("version").toString() != offer.value("installedVersion").toString())
        return false;
    const QString hash = offer.value("installedRootHash").toString();
    return hash.isEmpty() || request.value("rootHash").toString() == hash;
}

// The dependency-resolution request the INSTALL GATE sends.
//
// Names only the package the user is being asked about, and lets the resolver
// walk the dependencies itself. That is also exactly what package_manager_ui
// sends when it performs the install, so the gate and the installer are
// answering the same question — the property the gate's honesty depends on.
//
// It must not pre-expand dependencies into the request, however tempting:
// everything in the input array comes back `topLevel: true` (the resolver's
// contract — "entries that came from the input array"), and the gate drops
// top-level entries as the subject of its own dialog. Pre-expanding therefore
// hides every dependency, and the dialog states "No other packages need to
// change" while the installer installs them. It also defeats the
// installed-set short-circuit, which is only applied to entries the caller
// did not name — so already-satisfied deps would be listed as changes.
//
// OptionalDependencyPreview applies per-row pins only after discovering the
// reachable graph; it uses this same subject-only request for its first pass.
inline QString gateResolverRequest(const QString& name,
                                   const QString& repositoryUrl,
                                   const QString& version)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), name);
    if (!repositoryUrl.isEmpty())
        obj.insert(QStringLiteral("repositoryUrl"), repositoryUrl);
    if (!version.isEmpty())
        obj.insert(QStringLiteral("version"), version);

    QJsonArray arr;
    arr.append(obj);
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

inline QString installedPackagesJson(const QVariantList& installed)
{
    QJsonArray arr;
    for (const QVariant& v : installed) {
        const QVariantMap m = v.toMap();
        // package_manager rows expose both `name` and `moduleName`; the
        // resolver wants the module name.
        const QString name = m.value(QStringLiteral("moduleName")).toString().isEmpty()
                             ? m.value(QStringLiteral("name")).toString()
                             : m.value(QStringLiteral("moduleName")).toString();
        const QString version = m.value(QStringLiteral("version")).toString();
        // The short-circuit keys on both; a half-formed entry is not a claim.
        if (name.isEmpty() || version.isEmpty()) continue;
        QJsonObject o;
        o.insert(QStringLiteral("name"), name);
        o.insert(QStringLiteral("version"), version);
        const QString rootHash =
            m.value(QStringLiteral("hashes")).toMap().value(QStringLiteral("root")).toString();
        if (!rootHash.isEmpty()) o.insert(QStringLiteral("rootHash"), rootHash);
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// The installed list to retry a catalog-only resolve with: `installedJson` plus
// the installed copy of each package `resolved` reports no catalog could serve,
// typically a module bundled with Basecamp while the catalog that lists it is
// disabled. The resolver then keeps that copy if it satisfies the range; every
// other package still resolves to its newest catalog release. Empty when a retry
// would change nothing: the failed package is the subject itself, is not
// installed, or is in the list already.
inline QString installedFallback(const QString& installedJson, const QVariantList& resolved,
                                 const QVariantList& installed, const QString& subject)
{
    QJsonArray list = QJsonDocument::fromJson(installedJson.toUtf8()).array();
    QStringList listed;
    for (const QJsonValue& v : list)
        listed.append(v.toObject().value(QStringLiteral("name")).toString());

    bool added = false;
    for (const QVariant& v : resolved) {
        const QVariantMap row = v.toMap();
        const QString name = row.value(QStringLiteral("name")).toString();
        if (!row.contains(QStringLiteral("error")) || name.isEmpty()
            || name == subject || listed.contains(name))
            continue;
        for (const QVariant& p : installed) {
            const QVariantMap m = p.toMap();
            const QString module = m.value(QStringLiteral("moduleName")).toString().isEmpty()
                                   ? m.value(QStringLiteral("name")).toString()
                                   : m.value(QStringLiteral("moduleName")).toString();
            if (module != name) continue;
            const QJsonArray entry =
                QJsonDocument::fromJson(installedPackagesJson(QVariantList{m}).toUtf8()).array();
            for (const QJsonValue& e : entry) {
                list.append(e);
                added = true;
            }
            listed.append(name);
            break;
        }
    }
    return added ? QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact))
                 : QString();
}

} // namespace logos
