#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

// Which build each dependency of a resolved plan is bound to, and which of
// them the app's own repository doesn't provide, so it can't pin them
// (basecamp#333). Header-only, so it is under test.
namespace logos::outside {

struct Dependency {
    QString name;
    QString requiredBy;
    QString version;
    QString rootHash;
    // Where a planned row comes from; empty for an installed copy.
    QString repositoryUrl;
    bool installedCopy = false;
    // Not listed by the app's repository.
    bool outside = false;
};

// name → {version, rootHash} of the installed copy, or an empty version.
using Installed = std::function<QPair<QString, QString>(const QString&)>;
// Whether the app's repository lists a package by that name.
using Listed = std::function<bool(const QString&)>;

inline QString edgeName(const QVariant& dep)
{
    return dep.typeId() == QMetaType::QString ? dep.toString()
                                              : dep.toMap().value(QStringLiteral("name")).toString();
}

// The dependencies of `subject` and of every package the plan (the resolver's
// rows) downloads, read from the subject row's `dependencyGraph`. One the plan
// has no row for is the installed copy the resolver kept; what that copy
// needs was bound when it was installed, so it isn't walked. A dependency
// neither planned nor installed is not here: the resolver refuses that plan.
inline QList<Dependency> boundDependencies(const QString& subject, const QVariantList& resolved,
                                           const Listed& listedByAppRepository,
                                           const Installed& installed)
{
    QVariantMap graph;
    QHash<QString, QVariantMap> rows;
    QStringList walked{subject};
    for (const QVariant& v : resolved) {
        const QVariantMap row = v.toMap();
        const QString name = row.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || rows.contains(name)) continue;
        rows.insert(name, row);
        walked << name;
        const QVariantMap edges = row.value(QStringLiteral("dependencyGraph")).toMap();
        for (auto it = edges.cbegin(); it != edges.cend(); ++it) graph.insert(it.key(), it.value());
    }
    walked.removeDuplicates();

    QList<Dependency> out;
    QSet<QString> seen{subject};
    for (const QString& parent : walked) {
        for (const QVariant& edge : graph.value(parent).toList()) {
            const QString name = edgeName(edge);
            if (name.isEmpty() || seen.contains(name)) continue;
            Dependency d;
            d.name = name;
            d.requiredBy = parent;
            if (rows.contains(name)) {
                const QVariantMap row = rows.value(name);
                d.version = row.value(QStringLiteral("version")).toString();
                d.rootHash = row.value(QStringLiteral("rootHash")).toString();
                d.repositoryUrl = row.value(QStringLiteral("repositoryUrl")).toString();
            } else {
                const auto copy = installed(name);
                if (copy.first.isEmpty()) continue;
                d.version = copy.first;
                d.rootHash = copy.second;
                d.installedCopy = true;
            }
            d.outside = !listedByAppRepository(name);
            seen.insert(name);
            out << d;
        }
    }
    return out;
}

// For the dialog: one map per outside dependency, {name, version, installed,
// repository} with `repository` named by `repositoryName`.
inline QVariantList forDialog(const QList<Dependency>& deps,
                              const std::function<QString(const QString&)>& repositoryName)
{
    QVariantList out;
    for (const Dependency& d : deps) {
        if (!d.outside) continue;
        out << QVariantMap{{QStringLiteral("name"), d.name},
                           {QStringLiteral("version"), d.version},
                           {QStringLiteral("installed"), d.installedCopy},
                           {QStringLiteral("repository"),
                            d.installedCopy ? QString() : repositoryName(d.repositoryUrl)}};
    }
    return out;
}

// One log line per installed copy used and per outside dependency, with the
// build's rootHash: a version alone doesn't say which build it is.
inline QStringList logLines(const QString& subject, const QString& appRepository,
                            const QList<Dependency>& deps)
{
    QStringList lines;
    const QString repo = appRepository.isEmpty() ? QStringLiteral("its repository") : appRepository;
    for (const Dependency& d : deps) {
        if (!d.installedCopy && !d.outside) continue;
        const QString build = QStringLiteral("%1 %2 (rootHash %3)")
            .arg(d.name, d.version, d.rootHash.isEmpty() ? QStringLiteral("unknown") : d.rootHash);
        if (d.installedCopy)
            lines << QStringLiteral("Install plan for %1: %2, needed by %3, is the installed copy%4")
                         .arg(subject, build, d.requiredBy,
                              d.outside ? QStringLiteral("; not provided by ") + repo : QString());
        else
            lines << QStringLiteral("Install plan for %1: %2, needed by %3, not provided by %4; from %5")
                         .arg(subject, build, d.requiredBy, repo, d.repositoryUrl);
    }
    return lines;
}

}  // namespace logos::outside
