#pragma once

#include <QString>
#include <QVariant>

namespace logos {

// How a single `dependencies[]` entry was understood.
enum class DependencyEntryKind {
    Name,          // the entry named a module; `name` holds it
    Unrecognised,  // the entry named nothing we can act on
};

struct DependencyEntry {
    DependencyEntryKind kind = DependencyEntryKind::Unrecognised;
    QString name;
};

// Reads a manifest `dependencies[]` entry for the module it names.
//
// Extracted verbatim from PluginLoader::loadCoreDependencies so the decision
// has a name and a test.
inline DependencyEntry readDependencyEntry(const QVariant& entry)
{
    const QString name = entry.toString();
    if (name.isEmpty())
        return {DependencyEntryKind::Unrecognised, QString()};
    return {DependencyEntryKind::Name, name};
}

} // namespace logos
