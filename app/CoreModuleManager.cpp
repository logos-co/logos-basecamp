#include "CoreModuleManager.h"

#include "logos_qt_host_core.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>
#include <QVariantList>

#include "logos_api_client.h"
#include "logos_types.h"

namespace {

QJsonValue variantToJsonValue(const QVariant& value)
{
    if (!value.isValid()) return QJsonValue();

    if (value.canConvert<LogosResult>()) {
        const LogosResult result = value.value<LogosResult>();
        QJsonObject object;
        object.insert(QStringLiteral("success"), result.success);
        object.insert(QStringLiteral("value"), variantToJsonValue(result.value));
        object.insert(QStringLiteral("error"), variantToJsonValue(result.error));
        return object;
    }

    switch (value.metaType().id()) {
    case QMetaType::QVariantMap: {
        QJsonObject object;
        const QVariantMap map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            object.insert(it.key(), variantToJsonValue(it.value()));
        }
        return object;
    }
    case QMetaType::QVariantList: {
        QJsonArray array;
        const QVariantList list = value.toList();
        for (const QVariant& item : list) {
            array.append(variantToJsonValue(item));
        }
        return array;
    }
    default:
        return QJsonValue::fromVariant(value);
    }
}

}

CoreModuleManager::CoreModuleManager(LogosAPI* logosAPI,
                                     logos::qt::QtLogosCore* core,
                                     QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
    , m_core(core)
    , m_statsTimer(new QTimer(this))
{
    // No fallback path: without the core facade every wrapper below would be
    // a null dereference on the first stats tick, 2 seconds after startup and
    // far from the cause. main() owns the object and must pass it down.
    if (!m_core) {
        qFatal("CoreModuleManager requires a QtLogosCore instance");
    }

    connect(m_statsTimer, &QTimer::timeout,
            this, &CoreModuleManager::updateModuleStats);
    m_statsTimer->start(2000);
}

CoreModuleManager::~CoreModuleManager()
{
    // Belt-and-braces: Qt parenting already stops/deletes the timer, but
    // calling stop() here guarantees no in-flight tick fires against a
    // half-destroyed object during child destruction of other siblings.
    if (m_statsTimer) m_statsTimer->stop();
}

QStringList CoreModuleManager::knownModules() const
{
    return m_core->knownModules();
}

QStringList CoreModuleManager::loadedModules() const
{
    return m_core->loadedModules();
}

bool CoreModuleManager::loadModule(const QString& name)
{
    return m_core->loadModule(name, /*withDependencies=*/true);
}

bool CoreModuleManager::unloadModule(const QString& name)
{
    return m_core->unloadModule(name, /*withDependents=*/false);
}

bool CoreModuleManager::unloadModuleWithDependents(const QString& name)
{
    return m_core->unloadModule(name, /*withDependents=*/true);
}

QVariantMap CoreModuleManager::moduleStats(const QString& name) const
{
    return m_moduleStats.value(name);
}

void CoreModuleManager::refresh()
{
    // Re-scan all module directories via the lib, then let the Modules tab
    // re-read the composed list through Q_PROPERTY.
    m_core->refreshModules();
    emit coreModulesChanged();
}

QString CoreModuleManager::getMethods(const QString& moduleName)
{
    if (!m_logosAPI) {
        return "[]";
    }

    LogosAPIClient* client = m_logosAPI->getClient(moduleName);
    if (!client || !client->isConnected()) {
        return "[]";
    }

    QVariant result = client->invokeRemoteMethod(moduleName, "getPluginMethods");
    if (result.canConvert<QJsonArray>()) {
        QJsonArray methods = result.toJsonArray();
        QJsonDocument doc(methods);
        return doc.toJson(QJsonDocument::Compact);
    }

    return "[]";
}

QString CoreModuleManager::getEvents(const QString& moduleName)
{
    if (!m_logosAPI) {
        return "[]";
    }

    LogosAPIClient* client = m_logosAPI->getClient(moduleName);
    if (!client || !client->isConnected()) {
        return "[]";
    }

    QVariant result = client->invokeRemoteMethod(moduleName, "getPluginEvents");
    if (result.canConvert<QJsonArray>()) {
        QJsonArray events = result.toJsonArray();
        QJsonDocument doc(events);
        return doc.toJson(QJsonDocument::Compact);
    }

    return "[]";
}

QString CoreModuleManager::callMethod(const QString& moduleName,
                                      const QString& methodName,
                                      const QString& argsJson)
{
    if (!m_logosAPI) {
        return "{\"error\": \"LogosAPI not available\"}";
    }

    LogosAPIClient* client = m_logosAPI->getClient(moduleName);
    if (!client || !client->isConnected()) {
        return "{\"error\": \"Module not connected\"}";
    }

    QJsonDocument argsDoc = QJsonDocument::fromJson(argsJson.toUtf8());
    QJsonArray argsArray = argsDoc.array();

    QVariantList args;
    for (const QJsonValue& val : argsArray) {
        args.append(val.toVariant());
    }

    QVariant result;
    if (args.isEmpty()) {
        result = client->invokeRemoteMethod(moduleName, methodName);
    } else if (args.size() == 1) {
        result = client->invokeRemoteMethod(moduleName, methodName, args[0]);
    } else if (args.size() == 2) {
        result = client->invokeRemoteMethod(moduleName, methodName, args[0], args[1]);
    } else if (args.size() == 3) {
        result = client->invokeRemoteMethod(moduleName, methodName, args[0], args[1], args[2]);
    } else {
        return "{\"error\": \"Too many arguments\"}";
    }

    QJsonObject wrapper;
    wrapper["result"] = variantToJsonValue(result);
    QJsonDocument resultDoc(wrapper);
    return resultDoc.toJson(QJsonDocument::Compact);
}

void CoreModuleManager::updateModuleStats()
{
    // ONE call for the whole set. Do not rewrite this as moduleStats(name)
    // per module: each of those repeats the same full C call and full parse
    // (logos_qt_host_core.h says so), and MainUIBackend walks every known
    // module on every one of these 2-second ticks.
    const QVariantList allStats = m_core->allStats();

    for (const QVariant& entry : allStats) {
        const QVariantMap moduleObj = entry.toMap();
        const QString name = moduleObj.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            continue;
        }

        QVariantMap stats;
        // Tolerate multiple field names across runtime versions. Older
        // lib builds emit `cpu` / `memory` / `memory_MB`; newer ones
        // emit `cpu_percent` / `memory_mb` — which the facade models as
        // `cpuPercent` / `memoryMb` while still passing every raw key
        // through, so the old spellings remain readable here. Take the
        // first non-zero hit.
        double cpu = moduleObj.value(QStringLiteral("cpuPercent")).toDouble();
        if (cpu == 0) cpu = moduleObj.value(QStringLiteral("cpu")).toDouble();

        double memory = moduleObj.value(QStringLiteral("memoryMb")).toDouble();
        if (memory == 0) memory = moduleObj.value(QStringLiteral("memory")).toDouble();
        if (memory == 0) memory = moduleObj.value(QStringLiteral("memory_MB")).toDouble();

        // "cpu"/"memory" as 1-decimal STRINGS is the QML-facing contract, and
        // it stops here: MainUIBackend::buildCoreModulesSnapshot copies these
        // two keys straight through, ModuleInstanceModel exposes them as the
        // cpu/memory roles, and both fall back to the literal "0.0". The
        // facade's cpuPercent/memoryMb renaming must not leak past this line.
        stats[QStringLiteral("cpu")] = QString::number(cpu, 'f', 1);
        stats[QStringLiteral("memory")] = QString::number(memory, 'f', 1);
        m_moduleStats[name] = stats;
    }

    // Emitted unconditionally, where the hand-rolled version returned early on
    // a null or unparseable blob. Those two paths were unreachable — the
    // producer always returns a valid JSON array (process_stats.cpp:133,165) —
    // and an empty array already reached this emit before.
    emit coreModulesChanged();
}
