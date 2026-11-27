#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

// What an install or upgrade stops by replacing installed packages. Basecamp
// unloads it before it answers: the installer replaces the files under a
// running package otherwise. Header-only, so it is under test.
namespace logos::replace {

using Test = std::function<bool(const QString&)>;

// The packages a plan replaces: its rows ({name, version, rootHash}) that
// `replaces` says take an installed release's place. The installer fetches
// no row already on disk as it is.
inline QStringList replacedBy(const QVariantList& plan,
                              const std::function<bool(const QVariantMap&)>& replaces)
{
    QStringList names;
    for (const QVariant& v : plan) {
        const QVariantMap row = v.toMap();
        const QString name = row.value(QStringLiteral("name")).toString();
        if (!name.isEmpty() && replaces(row) && !names.contains(name)) names << name;
    }
    return names;
}

// What replacing `name` stops: it, if it runs, and each of its `dependents`
// that runs.
inline QStringList closedBy(const QString& name, const QStringList& dependents, const Test& running)
{
    QStringList names;
    if (running(name)) names << name;
    for (const QString& d : dependents)
        if (running(d) && !names.contains(d)) names << d;
    return names;
}

// Change rows ({name, action, …}) with `closes` on each that replaces an
// installed release: what replacing it stops.
inline QVariantList withCloses(QVariantList rows, const std::function<QStringList(const QString&)>& closed)
{
    for (QVariant& v : rows) {
        QVariantMap row = v.toMap();
        const QString action = row.value(QStringLiteral("action")).toString();
        if (action != QLatin1String("upgrade") && action != QLatin1String("downgrade")
            && action != QLatin1String("reinstall"))
            continue;
        const QStringList closes = closed(row.value(QStringLiteral("name")).toString());
        if (closes.isEmpty()) continue;
        row.insert(QStringLiteral("closes"), closes);
        v = row;
    }
    return rows;
}

}  // namespace logos::replace
