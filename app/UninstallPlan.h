#pragma once

// Composes and explains an uninstall batch.
//
//   composeFrom(in) — "user wants X gone; what else can go with it?"
//                     Returns the batch for requestMultiUninstall.
//   explainOf(in)   — narrates a batch the module has handed back.
//
// Algorithm, given targets T:
//   candidates = T ∪ ⋃ dependencies[t]     (installed only)
//   roots      = installed \ candidates
//   survivors  = ⋃ ({r} ∪ dependencies[r]) for r in roots
//   orphans    = candidates \ T \ survivors \ embedded \ protected
//   batch      = compose ? T ∪ orphans : T
//   kept       = candidates \ batch
//   dependents = (⋃ dependents[t]) \ candidates
//
// Cached dependency lists are already recursive closures, so the union over
// roots is a fixpoint — no iteration.
//
// Impl in UninstallPlan.cpp; tests depend on it via `srcdeps:` in
// uninstall_test.cpp.

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace uninstallplan {

enum class KeptReason {
    Embedded,   // built in.
    Protected,  // Basecamp-owned (main_ui).
    RequiredBy, // outside-batch consumer.
    Unused,     // orphaned but this pass isn't expanding orphans.
};

// Enum → wire string for QML: "embedded" / "protected" / "requiredBy" / "unused".
QString reasonName(KeptReason r);

struct Input {
    QStringList targets;

    // Recursive fwd/rev closures from package_manager.
    QHash<QString, QStringList> dependencies;
    QHash<QString, QStringList> dependents;
    QStringList                 installed;
    QSet<QString>               embedded;
    QSet<QString>               protectedNames;

    // Row-render decoration; missing entries fall back to name / "".
    QHash<QString, QString> displayNames;
    QHash<QString, QString> versions;
    QSet<QString>           loaded;
};

struct Row {
    QString name;
    QString displayName;
    QString version;
    bool    isTarget = false;
    bool    isLoaded = false;
};

struct KeptRow {
    QString     name;
    QString     displayName;
    KeptReason  reason = KeptReason::Unused;
    QStringList requiredBy;   // only set when reason == RequiredBy.
};

struct Plan {
    QStringList    batch;
    QList<Row>     removable;
    QList<KeptRow> kept;
    QList<Row>     dependents;
};

Plan composeFrom(const Input& in);  // orphan-expanding
Plan explainOf  (const Input& in);  // narration (orphans land in Kept as Unused)

}  // namespace uninstallplan
