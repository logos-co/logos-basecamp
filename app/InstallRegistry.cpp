#include "InstallRegistry.h"

InstallRegistry::InstallRegistry(QObject* parent) : QObject(parent) {}

int InstallRegistry::stage(const QString& name) const
{
    const auto it = m_ops.constFind(name);
    return it == m_ops.cend() ? InstallStage::None
                              : static_cast<int>(it->stage);
}

bool InstallRegistry::isInFlight(const QString& name) const
{
    const auto it = m_ops.constFind(name);
    if (it == m_ops.cend()) return false;
    switch (it->stage) {
    case InstallStage::Downloading:
    case InstallStage::Queued:
    case InstallStage::Installing:
        return true;
    default:
        return false;
    }
}

QString InstallRegistry::error(const QString& name) const
{
    const auto it = m_ops.constFind(name);
    return it == m_ops.cend() ? QString() : it->error;
}

QString InstallRegistry::targetVersion(const QString& name) const
{
    const auto it = m_ops.constFind(name);
    return it == m_ops.cend() ? QString() : it->targetVersion;
}

QString InstallRegistry::targetHash(const QString& name) const
{
    const auto it = m_ops.constFind(name);
    return it == m_ops.cend() ? QString() : it->targetHash;
}

void InstallRegistry::begin(const QString& name,
                       const QString& targetVersion,
                       const QString& targetHash,
                       const QString& startedByTopLevel)
{
    if (name.isEmpty()) return;
    const bool added = !m_ops.contains(name);
    Entry& e = m_ops[name];
    e.name              = name;
    e.targetVersion     = targetVersion;
    e.targetHash        = targetHash;
    e.stage             = InstallStage::Downloading;
    e.error.clear();
    e.startedByTopLevel = startedByTopLevel;
    emit stageChanged(name, e.stage);
    if (added) emit activeNamesChanged();
}

void InstallRegistry::setStage(const QString& name, InstallStage::Value stage)
{
    auto it = m_ops.find(name);
    if (it == m_ops.end()) return;
    if (it->stage == stage) return;
    it->stage = stage;
    emit stageChanged(name, stage);
}

void InstallRegistry::fail(const QString& name, const QString& error)
{
    auto it = m_ops.find(name);
    if (it == m_ops.end()) return;
    const bool stageChanged_ = it->stage != InstallStage::Failed;
    it->stage = InstallStage::Failed;
    it->error = error;
    if (stageChanged_) emit stageChanged(name, InstallStage::Failed);
    emit errorChanged(name, error);
}

void InstallRegistry::finish(const QString& name)
{
    setStage(name, InstallStage::Installed);
}

void InstallRegistry::clear(const QString& name)
{
    if (!m_ops.contains(name)) return;
    m_ops.remove(name);
    emit stageChanged(name, InstallStage::None);
    emit errorChanged(name, QString());
    emit activeNamesChanged();
}

void InstallRegistry::clearByTopLevel(const QString& topLevelName)
{
    if (topLevelName.isEmpty()) return;
    QStringList toRemove;
    for (auto it = m_ops.cbegin(); it != m_ops.cend(); ++it) {
        if (it->startedByTopLevel == topLevelName) toRemove.append(it.key());
    }
    if (toRemove.isEmpty()) return;
    for (const QString& name : toRemove) m_ops.remove(name);
    for (const QString& name : toRemove) {
        emit stageChanged(name, InstallStage::None);
        emit errorChanged(name, QString());
    }
    emit activeNamesChanged();
}

void InstallRegistry::beginOrTrack(const QString& name,
                              const QString& targetVersion,
                              const QString& targetHash,
                              const QString& startedByTopLevel)
{
    if (name.isEmpty()) return;
    if (!m_ops.contains(name)) {
        begin(name, targetVersion, targetHash, startedByTopLevel);
        return;
    }
    Entry& e = m_ops[name];
    if (!targetVersion.isEmpty()) e.targetVersion = targetVersion;
    if (!targetHash.isEmpty())    e.targetHash    = targetHash;
}
