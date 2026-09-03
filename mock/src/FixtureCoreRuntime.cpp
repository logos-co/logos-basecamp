#include "FixtureCoreRuntime.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QVariantMap>

#include <algorithm>

namespace {
QJsonObject objByName(const QJsonArray& modules, const QString& name)
{
    for (const QJsonValue& v : modules) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("name")).toString() == name) return o;
    }
    return {};
}
} // namespace

FixtureCoreRuntime::FixtureCoreRuntime(const QString& fixturePath)
{
    QFile f(fixturePath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "FixtureCoreRuntime: could not open" << fixturePath
                   << "— reporting zero modules";
        return;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "FixtureCoreRuntime: could not parse" << fixturePath
                   << "-" << err.errorString();
        return;
    }
    m_modules = doc.object().value(QStringLiteral("modules")).toArray();

    // "loaded" in the fixture is the INITIAL state; load/unload mutate m_loaded
    // from here, so get/load/unload stay coherent within a session.
    for (const QJsonValue& v : m_modules) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("loaded")).toBool())
            m_loaded.insert(o.value(QStringLiteral("name")).toString());
    }
}

void FixtureCoreRuntime::start()
{
    qInfo().noquote() << QStringLiteral(
        "FixtureCoreRuntime: %1 module(s) known, %2 pre-loaded: %3")
        .arg(m_modules.size()).arg(m_loaded.size())
        .arg(inFixtureOrder(m_loaded).join(QStringLiteral(", ")));
}

bool FixtureCoreRuntime::isKnown(const QString& name) const
{
    return !objByName(m_modules, name).isEmpty();
}

QStringList FixtureCoreRuntime::knownModules() const
{
    QStringList out;
    for (const QJsonValue& v : m_modules) {
        const QString n = v.toObject().value(QStringLiteral("name")).toString();
        if (!n.isEmpty()) out << n;
    }
    return out;
}

QStringList FixtureCoreRuntime::inFixtureOrder(const QSet<QString>& names) const
{
    QStringList out;
    for (const QString& n : knownModules())
        if (names.contains(n)) out << n;
    return out;
}

QStringList FixtureCoreRuntime::loadedModules() const { return inFixtureOrder(m_loaded); }

QStringList FixtureCoreRuntime::directDependencies(const QString& name) const
{
    QStringList deps;
    for (const QJsonValue& v : objByName(m_modules, name)
                                   .value(QStringLiteral("dependencies")).toArray()) {
        const QString d = v.toString();
        if (!d.isEmpty()) deps << d;
    }
    return deps;
}

QStringList FixtureCoreRuntime::transitiveDependencies(const QString& name) const
{
    QStringList ordered;
    QSet<QString> seen{name};
    QStringList frontier = directDependencies(name);
    while (!frontier.isEmpty()) {
        const QString cur = frontier.takeFirst();
        if (seen.contains(cur)) continue;
        seen.insert(cur);
        ordered << cur;
        frontier += directDependencies(cur);
    }
    return ordered;
}

QStringList FixtureCoreRuntime::directDependents(const QString& name) const
{
    QStringList out;
    for (const QString& cand : knownModules()) {
        if (cand == name) continue;
        if (directDependencies(cand).contains(name)) out << cand;
    }
    return out;
}

QStringList FixtureCoreRuntime::transitiveDependents(const QString& name) const
{
    QStringList ordered;
    QSet<QString> seen{name};
    QStringList frontier = directDependents(name);
    while (!frontier.isEmpty()) {
        const QString cur = frontier.takeFirst();
        if (seen.contains(cur)) continue;
        seen.insert(cur);
        ordered << cur;
        frontier += directDependents(cur);
    }
    return ordered;
}

bool FixtureCoreRuntime::loadModule(const QString& name, bool withDependencies)
{
    if (!isKnown(name)) {
        qWarning() << "FixtureCoreRuntime: unknown module" << name
                   << "- not in the fixture's `modules` array";
        return false;
    }
    if (withDependencies) {
        for (const QString& dep : transitiveDependencies(name)) {
            if (!isKnown(dep)) {
                qWarning() << "FixtureCoreRuntime: dependency" << dep << "of" << name
                           << "is not in the fixture - load failed";
                return false;
            }
            m_loaded.insert(dep);
        }
    }
    // "Ensure loaded": already-loaded is success. Callers rely on this.
    m_loaded.insert(name);
    return true;
}

bool FixtureCoreRuntime::unloadModule(const QString& name, bool withDependents)
{
    if (!m_loaded.contains(name)) return false;
    if (withDependents) {
        // transitiveDependents is a BFS outward, so reversing takes the
        // furthest dependents down first — leaves-first, as the real runtime
        // does, so nothing is briefly left pointing at a terminated parent.
        QStringList cascade = transitiveDependents(name);
        std::reverse(cascade.begin(), cascade.end());
        for (const QString& d : cascade) m_loaded.remove(d);
    }
    m_loaded.remove(name);
    return true;
}

void FixtureCoreRuntime::refreshModules()
{
    // The known set is fixed by the fixture, so a rescan finds nothing new.
    // The call still has to exist: the Modules tab's Reload button drives it.
}

QVariantList FixtureCoreRuntime::allStats() const
{
    QVariantList out;
    for (const QJsonValue& v : m_modules) {
        const QJsonObject mod = v.toObject();
        const QString name = mod.value(QStringLiteral("name")).toString();
        // Stats only mean something for a running module.
        if (name.isEmpty() || !m_loaded.contains(name)) continue;
        const QJsonObject s = mod.value(QStringLiteral("stats")).toObject();
        QVariantMap entry;
        entry[QStringLiteral("name")]        = name;
        entry[QStringLiteral("cpuPercent")]  = s.value(QStringLiteral("cpu_percent")).toDouble();
        entry[QStringLiteral("memoryMb")]    = s.value(QStringLiteral("memory_mb")).toDouble();
        out.append(entry);
    }
    return out;
}
