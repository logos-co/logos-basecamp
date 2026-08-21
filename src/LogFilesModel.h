#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantList>

// The session log files in the logs directory, newest session first, highest
// rotation first within a session. Fed by LogReaderWorker::filesListed via
// replaceRows(), which patches rows in place when the set of paths is
// unchanged — the listing is refreshed every few seconds while following,
// and a model reset each time would throw away the ListView's scroll
// position and hover state for a size that ticked up.
class LogFilesModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,
        StampRole,
        SessionLabelRole,    // "21 Aug 2026 10:04:23"
        RotationRole,        // 0 for the session's first file
        FileCountRole,       // files in this session (highest rotation + 1)
        SizeRole,            // bytes, as double for QML
        ModifiedRole,
        IsCurrentSessionRole,
        IsLiveRole,          // the file the running session writes right now
        StartsSessionRole,   // first row of its session in list order
    };
    Q_ENUM(Roles)

    explicit LogFilesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_rows.size(); }

    // Rows as the worker produced them (QVariantMaps with the role names as
    // keys), already sorted. Patches in place when paths match 1:1.
    void replaceRows(const QVariantList& rows);

    Q_INVOKABLE QVariantMap get(int row) const;
    // Path of the live file, or empty.
    QString livePath() const;
    // Path of the first row, or empty.
    QString firstPath() const;

signals:
    void countChanged();

private:
    struct Row {
        QString name;
        QString path;
        QString stamp;
        QString sessionLabel;
        int rotation = 0;
        int fileCount = 1;
        double size = 0;
        QDateTime modified;
        bool isCurrentSession = false;
        bool isLive = false;
        bool startsSession = false;
    };
    static Row toRow(const QVariantMap& m);
    static QList<int> diffRoles(const Row& a, const Row& b);

    QList<Row> m_rows;
};
