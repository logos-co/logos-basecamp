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
// what was required — and, for a signer, who actually published what is
// installed:
//
//     {"name":"depsvc","status":"version_mismatch","version":"1.0.0",
//      "requiredVersion":"^2.0.0","requiredSigner":"did:jwk:…",
//      "observedSigner":"did:jwk:…"}
//
// `status` is a closed vocabulary owned by logos-package-manager
// (DependencyStatus / dependencyStatusToString):
//
//     "installed" | "not_installed" | "cycle" | "version_mismatch"
//                 | "signer_mismatch" | "signer_unknown"
//
// This header decides which of those BLOCK a load, and turns a blocking row
// into something a user can act on. Route every read of a resolveFlatDeps row
// through here.
//
// The rule: classify by NAMING the statuses, never by exclusion. The gate
// used to read
//
//     if (m.value("status").toString() == "not_installed") missing << s;
//     else                                                 installed << s;
//
// so every status invented after that line was written landed in `installed`.
// That is how "version_mismatch" — an installed dependency the resolver had
// just rejected — came to count as a satisfied one, with no diagnostic
// anywhere. A switch over the vocabulary would have been a compile error to
// extend; a trailing `else` was silence.
//
// Deliberately NOT blocking, all three decisions rather than omissions:
//
//   "cycle" — a declared dependency cycle. Nothing here has been driven
//     against a real cyclic install, and blocking it would be a behaviour
//     change made blind. Pinned by a test so the next reader sees the choice.
//
//   "signer_unknown" — the edge pinned a publisher and NOTHING RECORDS who
//     published the installed package. That is absence of evidence, not
//     evidence of mismatch, and it is the EXPECTED state for two whole
//     populations: every embedded package (they are placed by the build and
//     never pass through the installer that records a publisher, so they can
//     never acquire one) and everything installed before the record existed.
//     Blocking on it would make a pin on an embedded dependency unsatisfiable
//     by construction, forever, with no action the user could take. The
//     package manager owns this call and can flip it in one place
//     (UnknownSignerPolicy::Strict makes the scanner emit signer_mismatch
//     instead, and then this gate blocks it without needing a change here) —
//     which is exactly why this gate should not second-guess it. Pinned by a
//     test.
//
//   an unrecognised status — this gate is an ADVISORY pre-check in front of
//     liblogos' own dependency resolver, run so the user gets "depsvc is not
//     installed" instead of a bare "plugin load failed". liblogos remains the
//     authority on whether a load succeeds. Refusing on a word this build
//     does not know would block loads that work; admitting one costs only the
//     nicer message.

enum class DependencyBlockKind {
    // The row does not block a load: satisfied, cyclic, a publisher we could
    // not check, or a status this build does not recognise.
    None,
    // Nothing is installed under this name.
    NotInstalled,
    // Something IS installed under this name, but its version does not
    // satisfy the range the depending manifest declared for this edge.
    VersionMismatch,
    // Something IS installed under this name and it is provably NOT the
    // package the dependant named: it was published by a different signer.
    //
    // A THIRD user action, not a variant of the other two. "Install it" and
    // "get a different version" both assume the thing on disk is the right
    // package; this one says it is not, and no version of what is installed
    // will do. Folding it into either would give the user an instruction that
    // cannot work.
    SignerMismatch,
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
    // The publisher DID the depending manifest PINNED for this edge. Empty
    // when it named no publisher, which is every manifest in the fleet today.
    QString requiredSigner;
    // The publisher DID the installed package's signature was VERIFIED
    // against. Empty when nothing recorded one — which is not the same as
    // "unsigned", and is why a SignerMismatch message must never be built
    // from this field alone.
    QString observedSigner;
};

inline DependencyBlocker readDependencyBlocker(const QVariant& row)
{
    const QVariantMap m = row.toMap();

    DependencyBlocker b;
    b.name             = m.value(QStringLiteral("name")).toString();
    b.requiredVersion  = m.value(QStringLiteral("requiredVersion")).toString();
    b.installedVersion = m.value(QStringLiteral("version")).toString();
    b.requiredSigner   = m.value(QStringLiteral("requiredSigner")).toString();
    b.observedSigner   = m.value(QStringLiteral("observedSigner")).toString();

    // Named, not excluded. Absence outranks everything — the same precedence
    // the scanner itself applies, and the right one: a constraint can only be
    // judged against something you have, and pointing a user at "requires
    // ^2.0.0" when nothing is installed sends them after the wrong remedy.
    //
    // The scanner has already resolved which single constraint failed, so
    // these are alternatives, not a priority list to re-derive here. Adding a
    // status means adding a branch, which is the point: the `else` that used
    // to sit here is how version_mismatch silently counted as satisfied.
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

// Whether the dependency is PRESENT on disk.
//
// A DIFFERENT question from whether the row blocks a load, and the one the
// dependency GRAPH asks — a version_mismatch or signer_mismatch row blocks a
// load and is on disk at the same time. Callers that build the graph (the
// uninstall plan's forward edges, the on-disk closure) must ask this one:
// dropping a mismatched dependency from the closure would leave the forward
// edge missing while resolveFlatDependents still reports the reverse edge, and
// a planner walking an asymmetric graph produces plans that are wrong in ways
// nobody traces back to here.
//
// Exactly one kind means "nothing under this name", which is why this one IS
// written by exclusion: every kind added later is by definition about a
// package that is installed, so it should count as present by default.
inline bool dependencyIsPresent(const DependencyBlocker& b)
{
    return b.kind != DependencyBlockKind::NotInstalled;
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

    case DependencyBlockKind::SignerMismatch:
        // A DIFFERENT sentence, because it is a different problem: the thing
        // on disk is somebody else's package that happens to share the name.
        // Never phrased as a version or an absence — no version of what is
        // installed will satisfy this, and reinstalling "it" gets the same
        // package back.
        //
        // Both DIDs when we have them: the pin alone says nothing about what
        // went wrong, and the observation alone reads as an accusation with
        // no charge. Base64url DIDs are long, so the caller renders them on
        // the row it already indents; keeping them out entirely would leave a
        // user with no way to tell WHICH publisher they ended up with.
        if (b.requiredSigner.isEmpty())
            return QStringLiteral("published by a different signer");
        if (b.observedSigner.isEmpty())
            return QStringLiteral("published by a different signer; requires %1")
                .arg(b.requiredSigner);
        return QStringLiteral("published by a different signer; requires %1, found %2")
            .arg(b.requiredSigner, b.observedSigner);

    case DependencyBlockKind::None:
        break;
    }
    return QString();
}

// The wire form handed to QML. `kind` reuses the module's own words for the
// same facts, so one status has one name from the scanner to the dialog.
inline QString dependencyBlockKindName(DependencyBlockKind k)
{
    // A switch, not a ternary chain. The chain this replaced was
    // `VersionMismatch ? "version_mismatch" : "not_installed"`, so every kind
    // added after it would have crossed into QML labelled "not_installed" —
    // the identical shape of the trailing `else` that made version_mismatch
    // count as satisfied in the first place, one layer out.
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
    m.insert(QStringLiteral("observedSigner"),   b.observedSigner);
    m.insert(QStringLiteral("detail"), dependencyBlockerDetail(b));
    return m;
}

// One word for a whole set of blockers, so a tile marker and a dialog
// headline can pick a shape without re-walking the list in QML.
//
//   ""         nothing blocks the load
//   "absent"   every blocker is a dependency that isn't installed
//   "mismatch" every blocker is an installed dependency of the wrong version
//   "signer"   every blocker is installed but published by somebody else
//   "mixed"    more than one of the above, and the copy must not claim it is
//              only one of them
//
// Classified by NAMING each kind, for the third time in this header and for
// the same reason each time: the loop this replaced tested one kind and swept
// everything else into `absent`, so a signer mismatch would have been
// summarised as "not installed" and the dialog would have told the user to go
// install a package that is sitting on their disk.
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
// consumers need it. Lives here rather than in the caller's async lambda so
// the split is under test — the two lists answer DIFFERENT questions and a
// mismatched dependency belongs in BOTH.
struct DependencyRowSplit {
    // On disk. The forward edges of the dependency graph the uninstall plan
    // walks — it must agree with the reverse edges resolveFlatDependents
    // reports, and those know nothing about version ranges.
    QStringList  present;
    // Refuses the load. Names only, for the consumers that just need to know
    // something is wrong (the tile marker, installStatus).
    QStringList  blocking;
    // The same set with the reason attached, for the one consumer that has to
    // tell the user what to do about it.
    QVariantList blockers;
};

inline DependencyRowSplit splitDependencyRows(const QVariantList& rows)
{
    DependencyRowSplit out;
    for (const QVariant& row : rows) {
        const DependencyBlocker b = readDependencyBlocker(row);
        // A row that names nothing is not a dependency. Dropping it is the
        // pre-existing behaviour and the only safe one — there is no name to
        // act on.
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
