#pragma once

#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QtQml/qqml.h>

// Filter proxy over LogLinesModel for Settings → Logs. Same shape as
// ModulesFilterProxy so the view code reads alike: QML instantiates it,
// points sourceModel at backend.logs.lines, and binds the knobs.
//
// - levels:     accepted level names; empty means "all".
// - sources:    accepted source names; empty means "all".
// - searchText: case-insensitive substring over message, source and
//               timestamp. Empty accepts everything.
// - visibleCount / totalCount feed the "N of M" readout in the panel header.
//
// Rows keep file order; there is deliberately no sort.
class LogFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QStringList levels     READ levels     WRITE setLevels     NOTIFY levelsChanged)
    Q_PROPERTY(QStringList sources    READ sources    WRITE setSources    NOTIFY sourcesChanged)
    Q_PROPERTY(QString     searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY visibleCountChanged)
    Q_PROPERTY(int totalCount   READ totalCount   NOTIFY totalCountChanged)
public:
    explicit LogFilterProxy(QObject* parent = nullptr);

    QStringList levels() const     { return m_levels; }
    QStringList sources() const    { return m_sources; }
    QString     searchText() const { return m_searchText; }
    int visibleCount() const { return rowCount(); }
    int totalCount() const;

    void setLevels(const QStringList& levels);
    void setSources(const QStringList& sources);
    void setSearchText(const QString& text);

    void setSourceModel(QAbstractItemModel* sourceModel) override;

    // Map a proxy row back to the source row (the 1-based line number is
    // sourceRow + 1). QML uses it to fetch the raw line for the detail pane.
    Q_INVOKABLE int sourceRow(int proxyRow) const;

    // The raw file text of visible rows first..last (inclusive, either
    // order, clamped), joined with '\n'. Rows that continue a ui-host bundle
    // (one physical line split for display) are emitted once, so a copy is
    // the file's own lines. maxLines > 0 truncates the result to that many
    // lines; the caller can tell from lineCount().
    Q_INVOKABLE QString textForRows(int first, int last, int maxLines = 0) const;
    // Number of physical lines textForRows(first, last) would emit.
    Q_INVOKABLE int lineCount(int first, int last) const;

signals:
    void levelsChanged();
    void sourcesChanged();
    void searchTextChanged();
    void visibleCountChanged();
    void totalCountChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QStringList m_levels;
    QStringList m_sources;
    QSet<QString> m_levelSet;
    QSet<QString> m_sourceSet;
    QString m_searchText;
    QString m_needle;
};
