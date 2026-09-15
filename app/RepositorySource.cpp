#include "RepositorySource.h"

RepositorySource repositorySourceFor(const QVariantMap& repo)
{
    const QString url = repo.value(QStringLiteral("url")).toString().trimmed();

    RepositorySource out;

    out.link = repo.value(QStringLiteral("homepage")).toString().trimmed();

    if (out.link.isEmpty()) {
        const QString owner = repo.value(QStringLiteral("sourceOwner")).toString().trimmed();
        const QString name  = repo.value(QStringLiteral("sourceRepo")).toString().trimmed();
        if (!owner.isEmpty() && !name.isEmpty())
            out.link = QStringLiteral("https://github.com/%1/%2").arg(owner, name);
    }

    if (out.link.isEmpty())
        out.link = url;

    out.label = repo.value(QStringLiteral("displayLabel")).toString().trimmed();
    if (out.label.isEmpty())
        out.label = url;

    return out;
}

QString repositoryLabelForEntry(const QVariantMap& catalogEntry,
                                const QVariantMap& repo)
{
    if (!repo.isEmpty())
        return repositorySourceFor(repo).label;

    const QString fromCatalog =
        catalogEntry.value(QStringLiteral("repositoryDisplayName")).toString().trimmed();
    if (!fromCatalog.isEmpty())
        return fromCatalog;

    return catalogEntry.value(QStringLiteral("repositoryUrl")).toString().trimmed();
}
