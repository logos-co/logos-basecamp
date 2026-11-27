#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>

// What an approved plan was made over, so it is not carried out on anything
// else: the release of every package it plans (or "" for none installed),
// and what depended on each installed one it replaces. Header-only, so it is
// under test.
namespace logos::planbasis {

struct Basis {
    QHash<QString, QString> releases;
    QHash<QString, QStringList> dependents;
};

// The first package of `basis` that is not as it was: another release, or
// something that depends on it now and didn't then, which the person was not
// told the plan closes or may break. "" when nothing changed.
inline QString changed(const Basis& basis,
                       const std::function<QString(const QString&)>& releaseNow,
                       const std::function<QStringList(const QString&)>& dependentsNow)
{
    for (auto it = basis.releases.cbegin(); it != basis.releases.cend(); ++it) {
        if (releaseNow(it.key()) != it.value()) return it.key();
    }
    for (auto it = basis.dependents.cbegin(); it != basis.dependents.cend(); ++it) {
        for (const QString& dependent : dependentsNow(it.key())) {
            if (!it.value().contains(dependent)) return it.key();
        }
    }
    return {};
}

}  // namespace logos::planbasis
