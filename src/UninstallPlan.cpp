#include "UninstallPlan.h"

namespace uninstallplan {

QString reasonName(KeptReason r)
{
    switch (r) {
    case KeptReason::Embedded:   return QStringLiteral("embedded");
    case KeptReason::Protected:  return QStringLiteral("protected");
    case KeptReason::RequiredBy: return QStringLiteral("requiredBy");
    case KeptReason::Unused:     return QStringLiteral("unused");
    }
    return QStringLiteral("unused");
}

namespace {

Row makeRow(const QHash<QString, QString>& displayNames,
            const QHash<QString, QString>& versions,
            const QSet<QString>&           loaded,
            const QString&                 name,
            bool                           isTarget)
{
    Row r;
    r.name        = name;
    // Handle both missing key and explicit empty-string binding.
    r.displayName = displayNames.value(name, name);
    if (r.displayName.isEmpty()) r.displayName = name;
    r.version     = versions.value(name);
    r.isTarget    = isTarget;
    r.isLoaded    = loaded.contains(name);
    return r;
}

Plan computeImpl(const Input& in, bool composeMode)
{
    const QSet<QString> installedSet(in.installed.cbegin(), in.installed.cend());

    // Targets: deduped, order-preserving, installed-only.
    QStringList   targets;
    QSet<QString> targetSet;
    for (const QString& t : in.targets) {
        if (t.isEmpty() || !installedSet.contains(t)) continue;
        if (!targetSet.contains(t)) { targetSet.insert(t); targets << t; }
    }

    // candidates = targets ∪ their installed forward closures.
    QStringList   candidates   = targets;
    QSet<QString> candidateSet = targetSet;
    for (const QString& t : targets) {
        for (const QString& d : in.dependencies.value(t)) {
            if (!installedSet.contains(d) || candidateSet.contains(d)) continue;
            candidateSet.insert(d);
            candidates << d;
        }
    }

    // Survivors: everything a root (installed but not in the closure) needs,
    // stays. requiredByOf remembers which roots protected each candidate,
    // for the popup's "still required by X" line.
    QSet<QString>               survivorSet;
    QHash<QString, QStringList> requiredByOf;
    for (const QString& r : in.installed) {
        if (candidateSet.contains(r)) continue;
        survivorSet.insert(r);
        for (const QString& d : in.dependencies.value(r)) {
            if (!installedSet.contains(d)) continue;
            survivorSet.insert(d);
            if (!candidateSet.contains(d)) continue;
            // find/insert instead of `operator[]` so we don't create
            // spurious empty entries as a side effect.
            auto it = requiredByOf.find(d);
            if (it == requiredByOf.end()) {
                requiredByOf.insert(d, QStringList{r});
            } else if (!it.value().contains(r)) {
                it.value() << r;
            }
        }
    }

    // Orphans: brought in by a target, wanted by nobody else, and legal to
    // remove.
    QStringList orphans;
    for (const QString& c : candidates) {
        if (targetSet.contains(c)) continue;
        if (survivorSet.contains(c)) continue;
        if (in.embedded.contains(c) || in.protectedNames.contains(c)) continue;
        orphans << c;
    }

    Plan plan;
    plan.batch = targets;
    if (composeMode) plan.batch += orphans;
    const QSet<QString> batchSet(plan.batch.cbegin(), plan.batch.cend());

    plan.removable.reserve(plan.batch.size());
    for (const QString& n : plan.batch) {
        plan.removable << makeRow(in.displayNames, in.versions, in.loaded,
                                  n, targetSet.contains(n));
    }

    // Kept = closure \ batch, each row tagged with why it survived.
    // Priority: Embedded > Protected > RequiredBy > Unused. Unused fires
    // only in explain mode; compose mode sweeps orphans into batch.
    for (const QString& c : candidates) {
        if (batchSet.contains(c)) continue;
        KeptRow k;
        k.name        = c;
        k.displayName = in.displayNames.value(c, c);
        if (k.displayName.isEmpty()) k.displayName = c;
        if (in.embedded.contains(c)) {
            k.reason = KeptReason::Embedded;
        } else if (in.protectedNames.contains(c)) {
            k.reason = KeptReason::Protected;
        } else if (auto it = requiredByOf.constFind(c);
                   it != requiredByOf.cend() && !it.value().isEmpty()) {
            k.reason     = KeptReason::RequiredBy;
            k.requiredBy = it.value();
        } else {
            k.reason = KeptReason::Unused;
        }
        plan.kept << k;
    }

    // Dependents: what breaks. Subtract anything already in the closure.
    QSet<QString> seenDependents;
    for (const QString& t : targets) {
        for (const QString& d : in.dependents.value(t)) {
            if (!installedSet.contains(d)) continue;
            if (candidateSet.contains(d) || seenDependents.contains(d)) continue;
            seenDependents.insert(d);
            plan.dependents << makeRow(in.displayNames, in.versions, in.loaded,
                                       d, /*isTarget=*/false);
        }
    }

    return plan;
}

}  // anonymous namespace

Plan composeFrom(const Input& in) { return computeImpl(in, /*composeMode=*/true); }
Plan explainOf (const Input& in) { return computeImpl(in, /*composeMode=*/false); }

}  // namespace uninstallplan
