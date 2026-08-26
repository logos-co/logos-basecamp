#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace logos {

// Reading a `package_manager.resolveFlatDependencies` row.
//
// The module answers with one row per transitive dependency:
//
//     {"name":"depsvc","status":"installed","version":"1.0.0","installType":"user"}
//
// and, when the depending manifest declared an object-form edge
// ({"name":…,"version":"^2.0.0","signer":"did:jwk:…"}), the row also carries
// what was required:
//
//     {"name":"depsvc","status":"version_mismatch","version":"1.0.0",
//      "requiredVersion":"^2.0.0","requiredSigner":"did:jwk:…"}
//
// `status` is a closed vocabulary owned by logos-package-manager
// (DependencyStatus / dependencyStatusToString):
//
//     "installed" | "not_installed" | "cycle" | "version_mismatch"
//
// This header decides which of those BLOCK a load, and turns a blocking row
// into something a user can act on.
//
// AS OF THIS COMMIT the predicate is a verbatim extraction of the one in
// PackageCoordinator::refreshDependencyCaches — status == "not_installed"
// blocks, everything else is treated as satisfied. It is lifted here
// unchanged, and under test, before it is changed.

enum class DependencyBlockKind {
    // The row does not block a load.
    None,
    // Nothing is installed under this name.
    NotInstalled,
    // Something IS installed under this name, but its version does not
    // satisfy the range the depending manifest declared for this edge.
    VersionMismatch,
};

struct DependencyBlocker {
    DependencyBlockKind kind = DependencyBlockKind::None;
    QString name;
    // The declared semver range, e.g. "^2.0.0". Empty when the manifest
    // declared a bare-name dependency — the edge with no constraint on it.
    QString requiredVersion;
    // The version actually present. Empty on a NotInstalled row: there is
    // nothing installed to report.
    QString installedVersion;
};

inline DependencyBlocker readDependencyBlocker(const QVariant& row)
{
    const QVariantMap m = row.toMap();

    DependencyBlocker b;
    b.name             = m.value(QStringLiteral("name")).toString();
    b.requiredVersion  = m.value(QStringLiteral("requiredVersion")).toString();
    b.installedVersion = m.value(QStringLiteral("version")).toString();

    const QString status = m.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("not_installed"))
        b.kind = DependencyBlockKind::NotInstalled;

    return b;
}

// The clause that tells the user what to DO about this row, without the
// module's display name — the caller owns that lookup and prepends it.
//
// A blocking row that names no constraint and no version would produce an
// empty clause; each branch keeps a fallback so the line never renders as a
// bare bullet with a dangling dash.
inline QString dependencyBlockerDetail(const DependencyBlocker& b)
{
    switch (b.kind) {
    case DependencyBlockKind::NotInstalled:
        // The declared range still rides along on an absent row, so the
        // message can say WHICH version to go and install.
        return b.requiredVersion.isEmpty()
            ? QStringLiteral("not installed")
            : QStringLiteral("not installed; requires %1").arg(b.requiredVersion);

    case DependencyBlockKind::VersionMismatch:
        if (b.requiredVersion.isEmpty())
            return QStringLiteral("installed version %1 was rejected")
                .arg(b.installedVersion);
        if (b.installedVersion.isEmpty())
            return QStringLiteral("requires %1").arg(b.requiredVersion);
        return QStringLiteral("requires %1, found %2")
            .arg(b.requiredVersion, b.installedVersion);

    case DependencyBlockKind::None:
        break;
    }
    return QString();
}

// The wire form handed to QML. `kind` reuses the module's own words for the
// same facts, so one status has one name from the scanner to the dialog.
inline QVariantMap dependencyBlockerToMap(const DependencyBlocker& b)
{
    QVariantMap m;
    m.insert(QStringLiteral("name"), b.name);
    m.insert(QStringLiteral("kind"),
             b.kind == DependencyBlockKind::VersionMismatch
                 ? QStringLiteral("version_mismatch")
                 : QStringLiteral("not_installed"));
    m.insert(QStringLiteral("requiredVersion"),  b.requiredVersion);
    m.insert(QStringLiteral("installedVersion"), b.installedVersion);
    m.insert(QStringLiteral("detail"), dependencyBlockerDetail(b));
    return m;
}

// One word for a whole set of blockers, so a tile marker and a dialog
// headline can pick a shape without re-walking the list in QML.
//
//   ""         nothing blocks the load
//   "absent"   every blocker is a dependency that isn't installed
//   "mismatch" every blocker is an installed dependency of the wrong version
//   "mixed"    both, and the copy must not claim it is only one of them
inline QString summariseDependencyBlockers(const QVariantList& blockers)
{
    bool absent = false;
    bool mismatch = false;
    for (const QVariant& v : blockers) {
        if (v.toMap().value(QStringLiteral("kind")).toString()
            == QLatin1String("version_mismatch"))
            mismatch = true;
        else
            absent = true;
    }
    if (absent && mismatch) return QStringLiteral("mixed");
    if (mismatch)           return QStringLiteral("mismatch");
    if (absent)             return QStringLiteral("absent");
    return QString();
}

} // namespace logos
