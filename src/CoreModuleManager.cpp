#include "CoreModuleManager.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QVariantList>

#include "logos_api_client.h"

// The logos_core_* C API is forward-declared here rather than in a shared
// header so this is the only translation unit that links against it
// directly. Anywhere else that needs core-plugin state goes through the
// typed wrappers above.
extern "C" {
    char* logos_core_get_module_stats();
    char** logos_core_get_known_modules();
    char** logos_core_get_loaded_modules();
    int logos_core_load_module_with_dependencies(const char* module_name);
    int logos_core_unload_module(const char* module_name);
    // Cascade unload: tears down `module_name` and all currently-loaded
    // modules that depend on it, in leaves-first order. Returns 1 on full
    // success, 0 on any failure. Remote mode only — a no-op in Local mode
    // today (Basecamp gates the UI off there).
    int logos_core_unload_module_with_dependents(const char* module_name);
    void logos_core_refresh_modules();
}

namespace {

// Drain a NULL-terminated char** handed back by the lib and release every
// entry plus the outer array. liblogos allocates these with new char[] /
// new char*[] (see module_manager.cpp::toNullTerminatedArray), so delete[]
// is the correct deallocator — do NOT use free(). Used by the knownModules
// / loadedModules wrappers.
QStringList drainCStringArray(char** arr)
{
    QStringList out;
    if (!arr) return out;
    for (char** p = arr; *p != nullptr; ++p) {
        out << QString::fromUtf8(*p);
        delete[] *p;
    }
    delete[] arr;
    return out;
}

}

CoreModuleManager::CoreModuleManager(LogosAPI* logosAPI, QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
    , m_statsTimer(new QTimer(this))
{
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
    return drainCStringArray(logos_core_get_known_modules());
}

QStringList CoreModuleManager::loadedModules() const
{
    return drainCStringArray(logos_core_get_loaded_modules());
}

bool CoreModuleManager::loadModule(const QString& name)
{
    return logos_core_load_module_with_dependencies(name.toUtf8().constData()) == 1;
}

bool CoreModuleManager::unloadModule(const QString& name)
{
    return logos_core_unload_module(name.toUtf8().constData()) == 1;
}

bool CoreModuleManager::unloadModuleWithDependents(const QString& name)
{
    return logos_core_unload_module_with_dependents(name.toUtf8().constData()) == 1;
}

QVariantMap CoreModuleManager::moduleStats(const QString& name) const
{
    return m_moduleStats.value(name);
}

void CoreModuleManager::refresh()
{
    // Re-scan all module directories via the lib, then let the Modules tab
    // re-read the composed list through Q_PROPERTY.
    logos_core_refresh_modules();
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
    wrapper["result"] = QJsonValue::fromVariant(result);
    QJsonDocument resultDoc(wrapper);
    return resultDoc.toJson(QJsonDocument::Compact);
}

void CoreModuleManager::updateModuleStats()
{
    char* stats_json = logos_core_get_module_stats();
    if (!stats_json) {
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(stats_json);
    free(stats_json);

    if (doc.isNull()) {
        qWarning() << "Failed to parse module stats JSON";
        return;
    }

    QJsonArray modulesArray;
    if (doc.isArray()) {
        modulesArray = doc.array();
    } else if (doc.isObject()) {
        QJsonObject root = doc.object();
        modulesArray = root["modules"].toArray();
    }

    for (const QJsonValue& val : modulesArray) {
        QJsonObject moduleObj = val.toObject();
        QString name = moduleObj["name"].toString();

        if (!name.isEmpty()) {
            QVariantMap stats;
            // Tolerate multiple field names across runtime versions. Older
            // lib builds emit `cpu` / `memory` / `memory_MB`; newer ones
            // emit `cpu_percent` / `memory_mb`. Take the first non-zero hit.
            double cpu = moduleObj["cpu_percent"].toDouble();
            if (cpu == 0) cpu = moduleObj["cpu"].toDouble();

            double memory = moduleObj["memory_mb"].toDouble();
            if (memory == 0) memory = moduleObj["memory"].toDouble();
            if (memory == 0) memory = moduleObj["memory_MB"].toDouble();

            stats["cpu"] = QString::number(cpu, 'f', 1);
            stats["memory"] = QString::number(memory, 'f', 1);
            m_moduleStats[name] = stats;
        }
    }

    emit coreModulesChanged();
}
