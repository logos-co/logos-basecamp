#pragma once

#include <QMetaType>
#include <QString>
#include <QVariant>
#include <QVariantMap>

namespace logos {

// Reading a manifest `dependencies[]` entry: either a bare name,
// "wallet_module", or an object carrying that name alongside the constraints
// an installer resolves it by, {"name":…, "version":"^2.0.0", "signer":"did:…"}.
// The LGX spec allows either and both declare THE SAME EDGE; a loader wants
// the name.
//
// The trap this exists to close: QVariant::toString() on a QVariantMap returns
// a NULL QString — not an error, not a warning. So the obvious `dep.toString()`
// plus an isEmpty() skip drops every object-form entry in total silence, and
// the plugin mounts on a dependency that was never loaded. Route every
// `dependencies[]` read through here; logos-cpp-sdk states the same rule for
// QJsonValue in cpp-generator/metadata_dependencies.h.

enum class DependencyEntryKind {
    // The entry named a module; `name` holds it, non-empty.
    Name,
    // Neither a name string nor an object with a non-empty string "name".
    // Callers MUST report this: skipping it is how a declared dependency
    // disappears with no diagnostic anywhere.
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
    // QJsonObject are what a caller holds when it came through QJsonDocument or
    // a QHash-backed converter. QVariant::toMap() converts all three.
    case QMetaType::QVariantMap:
    case QMetaType::QVariantHash:
    case QMetaType::QJsonObject:
        name = entry.toMap().value(QStringLiteral("name")).toString();
        break;

    // Deliberately no default stringification: QVariant would turn 42 into the
    // module name "42", and a silent wrong answer is worse than a reported one.
    default:
        break;
    }

    if (name.isEmpty())
        return {DependencyEntryKind::Unrecognised, QString()};
    return {DependencyEntryKind::Name, name};
}

} // namespace logos
