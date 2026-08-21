#include "LogFilesModel.h"

#include <QVariantMap>

LogFilesModel::LogFilesModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int LogFilesModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant LogFilesModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row& r = m_rows.at(index.row());
    switch (role) {
    case NameRole:             return r.name;
    case PathRole:             return r.path;
    case StampRole:            return r.stamp;
    case SessionLabelRole:     return r.sessionLabel;
    case RotationRole:         return r.rotation;
    case FileCountRole:        return r.fileCount;
    case SizeRole:             return r.size;
    case ModifiedRole:         return r.modified;
    case IsCurrentSessionRole: return r.isCurrentSession;
    case IsLiveRole:           return r.isLive;
    case StartsSessionRole:    return r.startsSession;
    default:                   return {};
    }
}

QHash<int, QByteArray> LogFilesModel::roleNames() const
{
    return {
        { NameRole,             "name" },
        { PathRole,             "path" },
        { StampRole,            "stamp" },
        { SessionLabelRole,     "sessionLabel" },
        { RotationRole,         "rotation" },
        { FileCountRole,        "fileCount" },
        { SizeRole,             "size" },
        { ModifiedRole,         "modified" },
        { IsCurrentSessionRole, "isCurrentSession" },
        { IsLiveRole,           "isLive" },
        { StartsSessionRole,    "startsSession" },
    };
}

LogFilesModel::Row LogFilesModel::toRow(const QVariantMap& m)
{
    Row r;
    r.name             = m.value(QStringLiteral("name")).toString();
    r.path             = m.value(QStringLiteral("path")).toString();
    r.stamp            = m.value(QStringLiteral("stamp")).toString();
    r.sessionLabel     = m.value(QStringLiteral("sessionLabel")).toString();
    r.rotation         = m.value(QStringLiteral("rotation")).toInt();
    r.fileCount        = m.value(QStringLiteral("fileCount")).toInt();
    r.size             = m.value(QStringLiteral("size")).toDouble();
    r.modified         = m.value(QStringLiteral("modified")).toDateTime();
    r.isCurrentSession = m.value(QStringLiteral("isCurrentSession")).toBool();
    r.isLive           = m.value(QStringLiteral("isLive")).toBool();
    return r;
}

QList<int> LogFilesModel::diffRoles(const Row& a, const Row& b)
{
    QList<int> roles;
    if (a.name != b.name)                         roles << NameRole;
    if (a.stamp != b.stamp)                       roles << StampRole;
    if (a.sessionLabel != b.sessionLabel)         roles << SessionLabelRole;
    if (a.rotation != b.rotation)                 roles << RotationRole;
    if (a.fileCount != b.fileCount)               roles << FileCountRole;
    if (a.size != b.size)                         roles << SizeRole;
    if (a.modified != b.modified)                 roles << ModifiedRole;
    if (a.isCurrentSession != b.isCurrentSession) roles << IsCurrentSessionRole;
    if (a.isLive != b.isLive)                     roles << IsLiveRole;
    if (a.startsSession != b.startsSession)       roles << StartsSessionRole;
    return roles;
}

void LogFilesModel::replaceRows(const QVariantList& rows)
{
    QList<Row> incoming;
    incoming.reserve(rows.size());
    QString previousStamp;
    for (const QVariant& v : rows) {
        Row r = toRow(v.toMap());
        r.startsSession = r.stamp != previousStamp;
        previousStamp = r.stamp;
        incoming.append(r);
    }

    bool samePaths = incoming.size() == m_rows.size();
    for (int i = 0; samePaths && i < incoming.size(); ++i)
        samePaths = incoming.at(i).path == m_rows.at(i).path;

    if (!samePaths) {
        beginResetModel();
        m_rows = incoming;
        endResetModel();
        emit countChanged();
        return;
    }

    for (int i = 0; i < incoming.size(); ++i) {
        const QList<int> changed = diffRoles(m_rows.at(i), incoming.at(i));
        if (changed.isEmpty()) continue;
        m_rows[i] = incoming.at(i);
        const QModelIndex idx = index(i);
        emit dataChanged(idx, idx, changed);
    }
}

QVariantMap LogFilesModel::get(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_rows.size()) return m;
    const auto roles = roleNames();
    const QModelIndex idx = index(row);
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        m.insert(QString::fromUtf8(it.value()), data(idx, it.key()));
    return m;
}

QString LogFilesModel::livePath() const
{
    for (const Row& r : m_rows)
        if (r.isLive) return r.path;
    return QString();
}

QString LogFilesModel::firstPath() const
{
    return m_rows.isEmpty() ? QString() : m_rows.first().path;
}
