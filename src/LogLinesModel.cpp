#include "LogLinesModel.h"

#include <QVariantMap>

#include <algorithm>

LogLinesModel::LogLinesModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int LogLinesModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_lines.size();
}

QVariant LogLinesModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_lines.size())
        return {};
    const LogLine& l = m_lines.at(index.row());
    switch (role) {
    case TimestampRole:  return l.timestamp;
    case LevelRole:      return l.level;
    case SourceRole:     return l.source;
    case MessageRole:    return l.message;
    case RawRole:        return l.raw;
    case LineNumberRole: return index.row() + 1;
    case ContinuesBundleRole: return l.continuesBundle;
    default:             return {};
    }
}

QHash<int, QByteArray> LogLinesModel::roleNames() const
{
    return {
        { TimestampRole,  "timestamp" },
        { LevelRole,      "level" },
        { SourceRole,     "source" },
        { MessageRole,    "message" },
        { RawRole,        "raw" },
        { LineNumberRole, "lineNumber" },
        { ContinuesBundleRole, "continuesBundle" },
    };
}

QString LogLinesModel::rawAt(int row) const
{
    return (row >= 0 && row < m_lines.size()) ? m_lines.at(row).raw : QString();
}

QVariantMap LogLinesModel::lineAt(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_lines.size()) return m;
    const LogLine& l = m_lines.at(row);
    m.insert(QStringLiteral("timestamp"), l.timestamp);
    m.insert(QStringLiteral("level"), l.level);
    m.insert(QStringLiteral("source"), l.source);
    m.insert(QStringLiteral("message"), l.message);
    m.insert(QStringLiteral("raw"), l.raw);
    m.insert(QStringLiteral("lineNumber"), row + 1);
    return m;
}

void LogLinesModel::reset(const QList<LogLine>& lines)
{
    beginResetModel();
    m_lines = lines;
    m_sourceCounts.clear();
    m_levelCounts.clear();
    for (const LogLine& l : m_lines) tally(l, +1);
    endResetModel();
    emit countChanged();
    emit countsChanged();
}

void LogLinesModel::append(const QList<LogLine>& lines)
{
    if (lines.isEmpty()) return;
    const int first = m_lines.size();
    beginInsertRows({}, first, first + lines.size() - 1);
    m_lines.append(lines);
    for (const LogLine& l : lines) tally(l, +1);
    endInsertRows();
    emit countChanged();
    emit countsChanged();
}

void LogLinesModel::clear()
{
    if (m_lines.isEmpty() && m_sourceCounts.isEmpty()) return;
    reset({});
}

void LogLinesModel::tally(const LogLine& l, int delta)
{
    m_sourceCounts[l.source] += delta;
    m_levelCounts[l.level] += delta;
}

QVariantList LogLinesModel::toCountList(const QHash<QString, int>& counts, bool appFirst)
{
    QList<QPair<QString, int>> rows;
    rows.reserve(counts.size());
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        if (it.value() > 0) rows.append({ it.key(), it.value() });
    }
    std::sort(rows.begin(), rows.end(), [appFirst](const auto& a, const auto& b) {
        if (appFirst) {
            const bool aApp = a.first == LogLineNames::kSourceApp;
            const bool bApp = b.first == LogLineNames::kSourceApp;
            if (aApp != bApp) return aApp;
        }
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });

    QVariantList out;
    out.reserve(rows.size());
    for (const auto& r : rows) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), r.first);
        m.insert(QStringLiteral("count"), r.second);
        out.append(m);
    }
    return out;
}

QVariantList LogLinesModel::sourceCounts() const { return toCountList(m_sourceCounts, true); }
QVariantList LogLinesModel::levelCounts() const  { return toCountList(m_levelCounts, false); }
