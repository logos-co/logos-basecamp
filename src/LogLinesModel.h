#pragma once

#include "LogLine.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// The parsed lines of one log file, in file order (oldest first), exposed to
// QML by named role. LogFilterProxy sits on top for level / source / text
// filtering; this model only stores and counts.
//
// Counts per source and per level are kept incrementally so the filter chips
// in LogsView can show "package_downloader · 42" without a second pass over
// ten thousand rows on every append.
class LogLinesModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles {
        TimestampRole = Qt::UserRole + 1,
        LevelRole,
        SourceRole,
        MessageRole,
        RawRole,
        LineNumberRole,       // 1-based position in the loaded file
        ContinuesBundleRole,  // see LogLine::continuesBundle
    };
    Q_ENUM(Roles)

    explicit LogLinesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_lines.size(); }
    const LogLine& at(int row) const { return m_lines.at(row); }

    // QML-side accessors for the detail pane (the proxy hands QML a source
    // row; these resolve it without a registered role enum).
    Q_INVOKABLE QString rawAt(int row) const;
    Q_INVOKABLE QVariantMap lineAt(int row) const;

    // Replace everything (a new file was opened).
    void reset(const QList<LogLine>& lines);
    // Append lines read while following the live file.
    void append(const QList<LogLine>& lines);
    void clear();

    // [{ name, count }] sorted by count descending, then name. Sources carry
    // the fixed "basecamp" first so the chip row has a stable anchor.
    QVariantList sourceCounts() const;
    QVariantList levelCounts() const;

signals:
    void countChanged();
    // Emitted after reset/append/clear once the per-source and per-level
    // tallies are up to date.
    void countsChanged();

private:
    void tally(const LogLine& l, int delta);
    static QVariantList toCountList(const QHash<QString, int>& counts, bool appFirst);

    QList<LogLine> m_lines;
    QHash<QString, int> m_sourceCounts;
    QHash<QString, int> m_levelCounts;
};
