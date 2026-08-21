#include "LogFilterProxy.h"

#include "LogLinesModel.h"

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QStringList>
#include <QVariant>

#include <algorithm>

LogFilterProxy::LogFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);

    connect(this, &QAbstractItemModel::rowsInserted, this, &LogFilterProxy::visibleCountChanged);
    connect(this, &QAbstractItemModel::rowsRemoved,  this, &LogFilterProxy::visibleCountChanged);
    connect(this, &QAbstractItemModel::modelReset,   this, &LogFilterProxy::visibleCountChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &LogFilterProxy::visibleCountChanged);
}

int LogFilterProxy::totalCount() const
{
    QAbstractItemModel* src = sourceModel();
    return src ? src->rowCount() : 0;
}

void LogFilterProxy::setSourceModel(QAbstractItemModel* sourceModel)
{
    if (QAbstractItemModel* prev = this->sourceModel()) prev->disconnect(this);

    QSortFilterProxyModel::setSourceModel(sourceModel);

    if (sourceModel) {
        connect(sourceModel, &QAbstractItemModel::rowsInserted, this, &LogFilterProxy::totalCountChanged);
        connect(sourceModel, &QAbstractItemModel::rowsRemoved,  this, &LogFilterProxy::totalCountChanged);
        connect(sourceModel, &QAbstractItemModel::modelReset,   this, &LogFilterProxy::totalCountChanged);
    }
    emit totalCountChanged();
    emit visibleCountChanged();
}

void LogFilterProxy::setLevels(const QStringList& levels)
{
    if (levels == m_levels) return;
    m_levels = levels;
    m_levelSet = QSet<QString>(levels.cbegin(), levels.cend());
    invalidateFilter();
    emit levelsChanged();
    emit visibleCountChanged();
}

void LogFilterProxy::setSources(const QStringList& sources)
{
    if (sources == m_sources) return;
    m_sources = sources;
    m_sourceSet = QSet<QString>(sources.cbegin(), sources.cend());
    invalidateFilter();
    emit sourcesChanged();
    emit visibleCountChanged();
}

void LogFilterProxy::setSearchText(const QString& text)
{
    if (text == m_searchText) return;
    m_searchText = text;
    m_needle = text.trimmed().toLower();
    invalidateFilter();
    emit searchTextChanged();
    emit visibleCountChanged();
}

int LogFilterProxy::sourceRow(int proxyRow) const
{
    const QModelIndex idx = mapToSource(index(proxyRow, 0));
    return idx.isValid() ? idx.row() : -1;
}

namespace {
// Visit the distinct physical lines behind proxy rows first..last, in order.
template <typename Fn>
void forEachPhysicalLine(const QAbstractItemModel& proxy, int first, int last, Fn fn)
{
    if (first > last) std::swap(first, last);
    first = std::max(first, 0);
    last = std::min(last, proxy.rowCount() - 1);
    // A ui-host bundle is one physical line shown as several rows; rows after
    // the first carry continuesBundle and share the raw text. Only those are
    // folded — two modules (or one module twice) printing identical lines
    // must stay two lines.
    QString previous;
    bool havePrevious = false;
    for (int row = first; row <= last; ++row) {
        const QModelIndex idx = proxy.index(row, 0);
        const QString raw = proxy.data(idx, LogLinesModel::RawRole).toString();
        const bool continues = proxy.data(idx, LogLinesModel::ContinuesBundleRole).toBool();
        if (continues && havePrevious && raw == previous) continue;
        if (!fn(raw)) return;
        previous = raw;
        havePrevious = true;
    }
}
} // namespace

QString LogFilterProxy::textForRows(int first, int last, int maxLines) const
{
    QStringList lines;
    forEachPhysicalLine(*this, first, last, [&](const QString& raw) {
        if (maxLines > 0 && lines.size() >= maxLines) return false;
        lines.append(raw);
        return true;
    });
    return lines.join(QLatin1Char('\n'));
}

int LogFilterProxy::lineCount(int first, int last) const
{
    int n = 0;
    forEachPhysicalLine(*this, first, last, [&](const QString&) { ++n; return true; });
    return n;
}

bool LogFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    QAbstractItemModel* src = sourceModel();
    if (!src) return true;

    const QModelIndex idx = src->index(sourceRow, 0, sourceParent);

    if (!m_levelSet.isEmpty()
        && !m_levelSet.contains(src->data(idx, LogLinesModel::LevelRole).toString()))
        return false;

    const QString source = src->data(idx, LogLinesModel::SourceRole).toString();
    if (!m_sourceSet.isEmpty() && !m_sourceSet.contains(source))
        return false;

    if (m_needle.isEmpty()) return true;

    if (src->data(idx, LogLinesModel::MessageRole).toString().toLower().contains(m_needle))
        return true;
    if (source.toLower().contains(m_needle))
        return true;
    return src->data(idx, LogLinesModel::TimestampRole).toString().contains(m_needle);
}
