#pragma once

#include <QJsonArray>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace logos {

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

} // namespace logos
