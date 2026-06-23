#pragma once

#include "InstallEnums.h"

#include <QHash>
#include <QObject>
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
    };

    explicit InstallRegistry(QObject* parent = nullptr);

    Q_INVOKABLE bool has(const QString& name) const { return m_ops.contains(name); }
    Q_INVOKABLE int  stage(const QString& name) const;
    Q_INVOKABLE bool isInFlight(const QString& name) const;
    QString          error(const QString& name) const;
    QString          targetVersion(const QString& name) const;
    QString          targetHash(const QString& name) const;
    QStringList      activeNames() const { return m_ops.keys(); }

    void beginOrTrack(const QString& name,
                      const QString& targetVersion,
                      const QString& targetHash,
                      const QString& startedByTopLevel);
    void begin(const QString& name,
               const QString& targetVersion,
               const QString& targetHash,
               const QString& startedByTopLevel);
    void setStage(const QString& name, InstallStage::Value stage);
    void fail(const QString& name, const QString& error);
    void finish(const QString& name);
    void clear(const QString& name);
    void clearByTopLevel(const QString& topLevelName);

signals:
    void activeNamesChanged();
    void stageChanged(const QString& name, InstallStage::Value stage);
    void errorChanged(const QString& name, const QString& error);

private:
    QHash<QString, Entry> m_ops;
};
