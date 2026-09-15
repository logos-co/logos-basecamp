#pragma once

#include <QString>
#include <QVariantMap>

// Where a package came from, as something a person can act on: a name to
// recognise and a page to open.
struct RepositorySource {
    // What every other view calls this repository — "Logos Official (logos-co)".
    QString label;
    // A page the user can open to check. Declared where possible, derived only
    // where the derivation is exact; never a guess.
    QString link;
};

RepositorySource repositorySourceFor(const QVariantMap& repo);
QString repositoryLabelForEntry(const QVariantMap& catalogEntry,
                                const QVariantMap& repo);
