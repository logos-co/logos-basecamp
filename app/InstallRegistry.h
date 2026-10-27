#pragma once

#include "InstallEnums.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Registry of in-flight install operations. One entry per package name.
//
// A name is not unique across repositories — two repos can publish the
// same package — so each entry also records WHICH repo the operation
// targets. Views must gate on `belongsTo` before rendering an entry's
// state, or an install started from one repo lights up every repo's copy
// of that row. An entry with no recorded repository (a local .lgx, or a
// caller that can't attribute it) matches every row, which is the old
// behaviour and the safe default: better to show progress on one row too
// many than on none at all.
class InstallRegistry : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList activeNames READ activeNames NOTIFY activeNamesChanged)

public:
    struct Entry {
        QString             name;
        QString             repositoryUrl;   // empty = not attributable
        QString             targetVersion;
        QString             targetHash;
        InstallStage::Value stage = InstallStage::None;
        QString             error;
        QString             startedByTopLevel;
        QSet<QString>       topLevels;
        quint64             downloadReceived = 0;
        quint64             downloadTotal    = 0;
        bool                downloadComplete = false;
        QString             transport;
        bool                fellBackFromStorage = false;
    };

    explicit InstallRegistry(QObject* parent = nullptr);

    Q_INVOKABLE bool has(const QString& name) const { return m_ops.contains(name); }
    // Whether the row identified by (name, repositoryUrl) is the one this
    // operation belongs to. True when there is no entry, when the entry
    // records no repository, or when the two match.
    bool belongsTo(const QString& name, const QString& repositoryUrl) const;
    Q_INVOKABLE int  stage(const QString& name) const;
    Q_INVOKABLE bool isInFlight(const QString& name) const;
    QString          error(const QString& name) const;
    QString          targetVersion(const QString& name) const;
    QString          targetHash(const QString& name) const;
    quint64          downloadReceived(const QString& name) const;
    quint64          downloadTotal(const QString& name) const;
    QString          downloadTransport(const QString& name) const;
    bool             downloadFellBack(const QString& name) const;
    quint64          planDownloadReceived(const QString& name) const;
    quint64          planDownloadTotal(const QString& name) const;
    int              planStage(const QString& name) const;
    QString          planDownloadTransport(const QString& name) const;
    bool             planDownloadFellBack(const QString& name) const;
    QStringList      activeNames() const { return m_ops.keys(); }

    void beginOrTrack(const QString& name,
                      const QString& targetVersion,
                      const QString& targetHash,
                      const QString& startedByTopLevel,
                      const QString& repositoryUrl = {});
    void begin(const QString& name,
               const QString& targetVersion,
               const QString& targetHash,
               const QString& startedByTopLevel,
               const QString& repositoryUrl = {});
    // Aggregate-initialised positionally by callers and tests
    // ({name, version, rootHash, size}), so ADD NEW FIELDS AT THE END —
    // inserting one in the middle silently re-binds every existing
    // initialiser.
    struct PlannedPackage {
        QString name;
        QString version;
        QString rootHash;
        quint64 size = 0;
        QString repositoryUrl;   // the repo this entry resolved from
    };

    void beginPlan(const QString& topLevel, const QList<PlannedPackage>& plan);
    void setStage(const QString& name, InstallStage::Value stage);
    void setDownloadProgress(const QString& name, quint64 received, quint64 total,
                             const QString& transport = {});
    void fail(const QString& name, const QString& error);
    void finish(const QString& name);
    void clear(const QString& name);
    void clearByTopLevel(const QString& topLevelName);

signals:
    void activeNamesChanged();
    void stageChanged(const QString& name, InstallStage::Value stage);
    void errorChanged(const QString& name, const QString& error);
    void downloadProgressChanged(const QString& name);
    void planStageChanged(const QString& topLevel);

private:
    QHash<QString, Entry> m_ops;
};
