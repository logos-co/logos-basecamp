#include "RepositoryModel.h"

RepositoryModel::RepositoryModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int RepositoryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant RepositoryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row& r = m_rows.at(index.row());
    switch (role) {
    case UrlRole:          return r.url;
    case DisplayNameRole:  return r.displayName;
    case NameRole:         return r.name;
    case DescriptionRole:  return r.description;
    case EnabledRole:      return r.enabled;
    case IsDefaultRole:    return r.isDefault;
    case ResolveErrorRole: return r.resolveError;
    }
    return {};
}

QHash<int, QByteArray> RepositoryModel::roleNames() const
{
    return {
        {UrlRole,          "url"},
        {DisplayNameRole,  "displayName"},
        {NameRole,         "name"},
        {DescriptionRole,  "description"},
        {EnabledRole,      "enabled"},
        {IsDefaultRole,    "isDefault"},
        {ResolveErrorRole, "resolveError"},
    };
}

void RepositoryModel::replaceAll(const QVariantList& repositories)
{
    beginResetModel();
    m_rows.clear();
    m_indexByUrl.clear();
    m_rows.reserve(repositories.size());
    for (const QVariant& item : repositories) {
        const QVariantMap map = item.toMap();
        Row row;
        row.url = map.value(QStringLiteral("url")).toString();
        row.displayName = map.value(QStringLiteral("displayName")).toString();
        row.name = map.value(QStringLiteral("name")).toString();
        row.description = map.value(QStringLiteral("description")).toString();
        row.enabled = map.value(QStringLiteral("enabled"), true).toBool();
        row.isDefault = map.value(QStringLiteral("isDefault")).toBool();
        row.resolveError = map.value(QStringLiteral("resolveError")).toString();
        if (row.url.isEmpty()) continue;
        m_indexByUrl.insert(row.url, m_rows.size());
        m_rows.append(std::move(row));
    }
    endResetModel();
}

int RepositoryModel::rowOfUrl(const QString& url) const
{
    return m_indexByUrl.value(url, -1);
}
