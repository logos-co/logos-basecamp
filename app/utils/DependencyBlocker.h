#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace logos {

// Reading a `package_manager.resolveFlatDependencies` row — one row per
// transitive dependency, and an object-form edge also carries what was
// required plus what the installed signature says of itself. Verbatim wire
// payloads are in tests/dependency_gate_test.cpp.
//
// `status` is a closed vocabulary owned by logos-package-manager
// (DependencyStatus / dependencyStatusToString):
//
//     "installed" | "not_installed" | "cycle" | "version_mismatch"
//                 | "signer_mismatch" | "signer_unknown"
//
// Route every read of such a row through here, and classify by NAMING the
// statuses: a trailing `else` silently counts every status invented after it
// as satisfied.
//
// Deliberately NOT blocking, decisions rather than omissions:
//
//   "cycle" — never driven against a real cyclic install, so blocking would be
//     a behaviour change made blind. Pinned by a test.
//
//   "signer_unknown" — the pin could not be CHECKED: nothing records who
//     published the installed package. Absence of evidence, not evidence of
//     mismatch, and the normal state for every embedded package — the build
//     places them, so they never pass through the installer that writes a
//     manifest.sig and can never acquire one. Blocking would make a pin on an
//     embedded dependency unsatisfiable by construction. The package manager
//     owns this call (UnknownSignerPolicy::Strict emits signer_mismatch
//     instead, which this gate does block). Pinned by a test.
//
//   an unrecognised status — this gate is an advisory pre-check in front of
//     liblogos' own resolver, which remains the authority on whether a load
//     succeeds. Refusing on a word this build does not know would block loads
//     that work.

enum class DependencyBlockKind {
    // Satisfied, cyclic, a publisher we could not check, or a status this
    // build does not recognise.
    None,
    NotInstalled,
    VersionMismatch,
    // Installed and provably NOT the package the dependant named: the
    // signature does not verify under the pinned key. A THIRD user action —
    // no version of what is installed will do.
    SignerMismatch,
};

struct DependencyBlocker {
    DependencyBlockKind kind = DependencyBlockKind::None;
    QString name;
    // Declared semver range, e.g. "^2.0.0". Empty for a bare-name dependency.
    QString requiredVersion;
    // Empty on a NotInstalled row: there is nothing installed to report.
    QString installedVersion;
    // The signer DID the depending manifest PINNED. Empty when it named none,
    // which is every manifest in the fleet today.
    QString requiredSigner;
    // What the installed signature says of itself, once checked against the
    // key its own DID carries. Empty when no usable signature is installed,
    // which is not the same as "unsigned".
    //
    // NOT what the verdict was computed from — that comes from verifying under
    // the PIN's key, so this differing from `requiredSigner` is the normal
    // shape of a signer_mismatch row. Never re-derive the verdict by comparing
    // the two: a document supplies both DID and signature, so it can always be
    // made to agree with itself.
    QString signerDid;
};

inline DependencyBlocker readDependencyBlocker(const QVariant& row)
{
    const QVariantMap m = row.toMap();

    DependencyBlocker b;
    b.name             = m.value(QStringLiteral("name")).toString();
    b.requiredVersion  = m.value(QStringLiteral("requiredVersion")).toString();
    b.installedVersion = m.value(QStringLiteral("version")).toString();
    b.requiredSigner   = m.value(QStringLiteral("requiredSigner")).toString();
    b.signerDid        = m.value(QStringLiteral("signerDid")).toString();

    // Named, not excluded. The scanner has already resolved which single
    // constraint failed, so these are alternatives, not a priority list to
    // re-derive here; adding a status means adding a branch.
    const QString status = m.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("not_installed"))
        b.kind = DependencyBlockKind::NotInstalled;
    else if (status == QLatin1String("version_mismatch"))
        b.kind = DependencyBlockKind::VersionMismatch;
    else if (status == QLatin1String("signer_mismatch"))
        b.kind = DependencyBlockKind::SignerMismatch;
    // "signer_unknown" is deliberately absent — see the header comment.

    return b;
}

// Whether the dependency is PRESENT on disk — a different question from
// whether the row blocks a load, and a version_mismatch row is both. Callers
// building the dependency GRAPH must ask this one: dropping a mismatched
// dependency leaves the forward edge missing while resolveFlatDependents still
// reports the reverse one, and a planner walking an asymmetric graph produces
// wrong plans nobody traces back to here.
//
// Written by exclusion on purpose: every kind added later is about a package
// that IS installed.
inline bool dependencyIsPresent(const DependencyBlocker& b)
{
    return b.kind != DependencyBlockKind::NotInstalled;
}

// The clause telling the user what to DO about this row, without the module's
// display name — the caller prepends that. Every branch keeps a fallback so a
// row naming neither constraint nor version never renders as a dangling dash.
inline QString dependencyBlockerDetail(const DependencyBlocker& b)
{
    switch (b.kind) {
    case DependencyBlockKind::NotInstalled:
        // The declared range rides along even on an absent row, so the message
        // can say WHICH version to install.
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

    case DependencyBlockKind::SignerMismatch:
        // Never phrased as a version or an absence: no version of what is
        // installed satisfies this, and reinstalling gets the same package
        // back. Both DIDs when we have them, or the user cannot tell whose
        // package they ended up with.
        if (b.requiredSigner.isEmpty())
            return QStringLiteral("signed by a different key");
        if (b.signerDid.isEmpty())
            return QStringLiteral("not signed by the required key; requires %1")
                .arg(b.requiredSigner);
        return QStringLiteral("signed by a different key; requires %1, signed by %2")
            .arg(b.requiredSigner, b.signerDid);

    case DependencyBlockKind::None:
        break;
    }
    return QString();
}

// The wire form handed to QML. `kind` reuses the module's own words, so one
// status has one name from the scanner to the dialog.
inline QString dependencyBlockKindName(DependencyBlockKind k)
{
    // A switch, not a ternary chain: a chain labels every kind added after it
    // "not_installed" on the way into QML.
    switch (k) {
    case DependencyBlockKind::VersionMismatch: return QStringLiteral("version_mismatch");
    case DependencyBlockKind::SignerMismatch:  return QStringLiteral("signer_mismatch");
    case DependencyBlockKind::NotInstalled:
    case DependencyBlockKind::None:            break;
    }
    return QStringLiteral("not_installed");
}

inline QVariantMap dependencyBlockerToMap(const DependencyBlocker& b)
{
    QVariantMap m;
    m.insert(QStringLiteral("name"), b.name);
    m.insert(QStringLiteral("kind"), dependencyBlockKindName(b.kind));
    m.insert(QStringLiteral("requiredVersion"),  b.requiredVersion);
    m.insert(QStringLiteral("installedVersion"), b.installedVersion);
    m.insert(QStringLiteral("requiredSigner"),   b.requiredSigner);
    m.insert(QStringLiteral("signerDid"),        b.signerDid);
    m.insert(QStringLiteral("detail"), dependencyBlockerDetail(b));
    return m;
}

// One word for a whole set of blockers, so a tile marker and a dialog headline
// need not re-walk the list in QML.
//
//   ""         nothing blocks the load
//   "absent"   every blocker is a dependency that isn't installed
//   "mismatch" every blocker is an installed dependency of the wrong version
//   "signer"   every blocker is installed but published by somebody else
//   "mixed"    more than one of the above; the copy must not claim one
//
// Named, not excluded, for the same reason as above: sweeping unknown kinds
// into `absent` summarises a signer mismatch as "not installed".
inline QString summariseDependencyBlockers(const QVariantList& blockers)
{
    bool absent = false;
    bool mismatch = false;
    bool signer = false;
    for (const QVariant& v : blockers) {
        const QString kind = v.toMap().value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("version_mismatch"))     mismatch = true;
        else if (kind == QLatin1String("signer_mismatch")) signer = true;
        else                                               absent = true;
    }
    const int shapes = (absent ? 1 : 0) + (mismatch ? 1 : 0) + (signer ? 1 : 0);
    if (shapes > 1)  return QStringLiteral("mixed");
    if (signer)      return QStringLiteral("signer");
    if (mismatch)    return QStringLiteral("mismatch");
    if (absent)      return QStringLiteral("absent");
    return QString();
}

// One module's whole resolveFlatDependencies reply, split the way its
// consumers need it. Here rather than in the caller's async lambda so the
// split is under test — the two lists answer DIFFERENT questions and a
// mismatched dependency belongs in BOTH.
struct DependencyRowSplit {
    // On disk. Forward edges of the graph the uninstall plan walks; must agree
    // with the reverse edges resolveFlatDependents reports, which know nothing
    // about version ranges.
    QStringList  present;
    // Refuses the load. Names only, for consumers that need no reason.
    QStringList  blocking;
    // The same set with the reason attached.
    QVariantList blockers;
};

inline DependencyRowSplit splitDependencyRows(const QVariantList& rows)
{
    DependencyRowSplit out;
    for (const QVariant& row : rows) {
        const DependencyBlocker b = readDependencyBlocker(row);
        // A row that names nothing is not a dependency: no name to act on.
        if (b.name.isEmpty()) continue;

        if (dependencyIsPresent(b)) out.present << b.name;

        if (b.kind != DependencyBlockKind::None) {
            out.blocking << b.name;
            out.blockers << dependencyBlockerToMap(b);
        }
    }
    return out;
}

} // namespace logos
