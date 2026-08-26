#pragma once

#include <QMetaType>
#include <QString>
#include <QVariant>
#include <QVariantMap>

namespace logos {

// Reading a manifest `dependencies[]` entry.
//
// An entry is either a bare name
//
//     "wallet_module"
//
// or an object carrying that name alongside the constraints an installer
// resolves it by
//
//     {"name": "wallet_module", "version": "^2.0.0", "signer": "did:jwk:…"}
//
// Both forms declare THE SAME EDGE — the LGX spec allows either, lgpm parses
// either, logos-module and logos-standalone-app already read either. A loader
// wants the name; the range and the signer are the installer's business.
//
// The trap this exists to close: QVariant::toString() on a QVariantMap
// returns a NULL QString. Not an error, not an exception, not a warning — an
// empty string. So the obvious reader
//
//     QString depName = dep.toString();
//     if (depName.isEmpty()) continue;
//
// skips every object-form entry in total silence, and the plugin mounts on
// top of a dependency that was never loaded. Route every `dependencies[]`
// read through here instead of calling toString() on the entry — the same
// rule logos-cpp-sdk states for its QJsonValue equivalent in
// cpp-generator/metadata_dependencies.h: a reader that walks the array itself
// decides on its own what an element names, and one that decides differently
// from its neighbours produces a disagreement that surfaces far from here.

// How a single entry was understood.
enum class DependencyEntryKind {
    // The entry named a module; `name` holds it, non-empty.
    Name,
    // The entry named nothing we can act on: a shape that is neither a name
    // string nor an object with a string "name", or one of those with the
    // name missing or empty. Callers MUST report this. Skipping it is how a
    // declared dependency disappears with no diagnostic anywhere.
    Unrecognised,
};

struct DependencyEntry {
    DependencyEntryKind kind = DependencyEntryKind::Unrecognised;
    QString name;
};

inline DependencyEntry readDependencyEntry(const QVariant& entry)
{
    QString name;

    switch (entry.typeId()) {
    case QMetaType::QString:
        name = entry.toString();
        break;

    // QVariantMap is what the module IPC hands back; QVariantHash and
    // QJsonObject are what a caller holds when it came through QJsonDocument
    // or a QHash-backed converter. Same entry, same answer. QVariant::toMap()
    // converts all three.
    case QMetaType::QVariantMap:
    case QMetaType::QVariantHash:
    case QMetaType::QJsonObject:
        name = entry.toMap().value(QStringLiteral("name")).toString();
        break;

    // Deliberately no default stringification. A number, a bool, a nested
    // list or a null is not a dependency entry, and QVariant would happily
    // turn 42 into the module name "42" — a silent wrong answer is worse than
    // a reported one.
    default:
        break;
    }

    if (name.isEmpty())
        return {DependencyEntryKind::Unrecognised, QString()};
    return {DependencyEntryKind::Name, name};
}

} // namespace logos
