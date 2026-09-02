#pragma once

#include "InstallEnums.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Registry of in-flight install operations. One entry per package name
class InstallRegistry : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList activeNames READ activeNames NOTIFY activeNamesChanged)

public:
    struct Entry {
        QString             name;
        QString             targetVersion;
        QString             targetHash;
        InstallStage::Value stage = InstallStage::None;
        QString             error;
        QString             startedByTopLevel;
        QSet<QString>       topLevels;
        quint64             downloadReceived = 0;
        quint64             downloadTotal    = 0;
        bool                downloadComplete = false;
    };

    explicit InstallRegistry(QObject* parent = nullptr);

    Q_INVOKABLE bool has(const QString& name) const { return m_ops.contains(name); }
    Q_INVOKABLE int  stage(const QString& name) const;
    Q_INVOKABLE bool isInFlight(const QString& name) const;
    QString          error(const QString& name) const;
    QString          targetVersion(const QString& name) const;
    QString          targetHash(const QString& name) const;
    quint64          downloadReceived(const QString& name) const;
    quint64          downloadTotal(const QString& name) const;
    quint64          planDownloadReceived(const QString& name) const;
    quint64          planDownloadTotal(const QString& name) const;
    int              planStage(const QString& name) const;
    QStringList      activeNames() const { return m_ops.keys(); }

    void beginOrTrack(const QString& name,
                      const QString& targetVersion,
                      const QString& targetHash,
                      const QString& startedByTopLevel);
    void begin(const QString& name,
               const QString& targetVersion,
               const QString& targetHash,
               const QString& startedByTopLevel);
    struct PlannedPackage {
        QString name;
        QString version;
        QString rootHash;
        quint64 size = 0;
    };

    void beginPlan(const QString& topLevel, const QList<PlannedPackage>& plan);
    void setStage(const QString& name, InstallStage::Value stage);
    void setDownloadProgress(const QString& name, quint64 received, quint64 total);
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
