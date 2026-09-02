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
    case InstallStage::Downloaded:
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

quint64 InstallRegistry::downloadReceived(const QString& name) const
{
    const auto it = m_ops.constFind(name);
    return it == m_ops.cend() ? 0 : it->downloadReceived;
}

quint64 InstallRegistry::downloadTotal(const QString& name) const
{
    const auto it = m_ops.constFind(name);
    return it == m_ops.cend() ? 0 : it->downloadTotal;
}

// Shared walk for the two plan accessors. `pick` selects which counter to
// sum. Returns the own-entry value when no entry names `topLevel`, so a
// dependency row (which nothing points at) reports itself.
static quint64 sumOverPlan(const QHash<QString, InstallRegistry::Entry>& ops,
                           const QString& topLevel,
                           quint64 (*pick)(const InstallRegistry::Entry&))
{
    quint64 sum = 0;
    bool matched = false;
    for (auto it = ops.cbegin(); it != ops.cend(); ++it) {
        if (!it->topLevels.contains(topLevel)) continue;
        matched = true;
        sum += pick(*it);
    }
    if (matched) return sum;
    const auto own = ops.constFind(topLevel);
    return own == ops.cend() ? 0 : pick(*own);
}

quint64 InstallRegistry::planDownloadReceived(const QString& name) const
{
    return sumOverPlan(m_ops, name,
                       [](const Entry& e) { return e.downloadReceived; });
}

quint64 InstallRegistry::planDownloadTotal(const QString& name) const
{
    return sumOverPlan(m_ops, name,
                       [](const Entry& e) { return e.downloadTotal; });
}

int InstallRegistry::planStage(const QString& name) const
{
    bool matched = false, failed = false, installing = false,
         onWire = false, fetched = false, installed = false;
    int  count = 0, installedCount = 0;
    for (auto it = m_ops.cbegin(); it != m_ops.cend(); ++it) {
        if (!it->topLevels.contains(name)) continue;
        matched = true;
        ++count;
        switch (it->stage) {
        case InstallStage::Failed:      failed = true;     break;
        case InstallStage::Installing:  installing = true; break;
        case InstallStage::Queued:
        case InstallStage::Downloading: onWire = true;     break;
        case InstallStage::Downloaded:  fetched = true;    break;
        case InstallStage::Installed:   installed = true; ++installedCount; break;
        case InstallStage::None:                           break;
        }
    }
    const bool allInstalled = (count > 0 && installedCount == count);
    if (!matched) {
        const auto own = m_ops.constFind(name);
        return own == m_ops.cend() ? int(InstallStage::None) : int(own->stage);
    }
    if (failed) return int(InstallStage::Failed);

    if (onWire) return int(InstallStage::Downloading);
    // Downloading -> Installing -> Downloading between packages.
    if (installing || installed)
        return allInstalled ? int(InstallStage::Installed)
                            : int(InstallStage::Installing);

    if (fetched) return int(InstallStage::Downloading);
    return int(InstallStage::None);
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
    if (!startedByTopLevel.isEmpty()) e.topLevels.insert(startedByTopLevel);
    emit stageChanged(name, e.stage);
    if (added) emit activeNamesChanged();
}

void InstallRegistry::beginPlan(const QString& topLevel,
                                const QList<PlannedPackage>& plan)
{
    if (topLevel.isEmpty()) return;
    bool added = false;
    for (const PlannedPackage& p : plan) {
        if (p.name.isEmpty()) continue;
        if (!m_ops.contains(p.name)) {
            added = true;
            Entry& e = m_ops[p.name];
            e.name          = p.name;
            e.targetVersion = p.version;
            e.targetHash    = p.rootHash;
            e.stage         = InstallStage::Queued;
            e.startedByTopLevel = topLevel;
            e.downloadTotal = p.size;
            e.topLevels.insert(topLevel);
            emit stageChanged(p.name, e.stage);
        } else {
            Entry& e = m_ops[p.name];
            e.topLevels.insert(topLevel);
            if (e.downloadTotal == 0 && p.size > 0) e.downloadTotal = p.size;
        }
        emit downloadProgressChanged(p.name);
    }
    if (added) emit activeNamesChanged();
    emit downloadProgressChanged(topLevel);
    emit planStageChanged(topLevel);
}

void InstallRegistry::setStage(const QString& name, InstallStage::Value stage)
{
    auto it = m_ops.find(name);
    if (it == m_ops.end()) return;
    if (it->stage == stage) return;
    it->stage = stage;
    emit stageChanged(name, stage);
    for (const QString& t : it->topLevels) emit planStageChanged(t);
}

void InstallRegistry::setDownloadProgress(const QString& name,
                                          quint64 received, quint64 total)
{
    auto it = m_ops.find(name);
    if (it == m_ops.end()) return;
    if (it->stage == InstallStage::Queued && !it->downloadComplete) {
        it->stage = InstallStage::Downloading;
        emit stageChanged(name, it->stage);
        for (const QString& t : it->topLevels) emit planStageChanged(t);
    }
    if (it->stage != InstallStage::Downloading) return;

    it->downloadReceived = received;
    if (total > 0) it->downloadTotal = total;
    if (it->downloadTotal > 0 && it->downloadReceived >= it->downloadTotal) {
        it->downloadComplete = true;
        if (it->stage == InstallStage::Downloading) {
            it->stage = InstallStage::Downloaded;
            emit stageChanged(name, it->stage);
            for (const QString& t : it->topLevels) emit planStageChanged(t);
        }
    }

    emit downloadProgressChanged(name);
    for (const QString& t : it->topLevels)
        if (t != name) emit downloadProgressChanged(t);
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
    for (auto it = m_ops.begin(); it != m_ops.end(); ++it) {
        if (!it->topLevels.contains(topLevelName)) continue;
        it->topLevels.remove(topLevelName);
        // Still part of another in-flight install (a shared dependency):
        // keep the entry, or that install loses the package mid-download.
        if (!it->topLevels.isEmpty()) {
            if (it->startedByTopLevel == topLevelName)
                it->startedByTopLevel = *it->topLevels.cbegin();
            continue;
        }
        toRemove.append(it.key());
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
    // Track the additional owner: an entry seeded by beginPlan, or shared
    // with another install, must still count toward this one's aggregate.
    if (!startedByTopLevel.isEmpty()
        && !e.topLevels.contains(startedByTopLevel)) {
        e.topLevels.insert(startedByTopLevel);
        emit downloadProgressChanged(startedByTopLevel);
    }
}
