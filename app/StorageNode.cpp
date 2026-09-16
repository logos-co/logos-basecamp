#include "StorageNode.h"

#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_sdk.h"

namespace {

constexpr int kStopTimeoutMs = 5000;

// The home is shared with the Storage UI.
QString sharedStorageHome()
{
    return QDir::homePath() + "/.logos_storage";
}

// The config is shared with the Storage UI, which reads this file to show and
// compare what the node runs with.
QJsonObject sharedUserNodeConfig()
{
    QFile file(sharedStorageHome() + "/config.json");

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject eventPayload(const QVariantList& data)
{
    // The payload is the first item in the data list, as a JSON string.
    auto payload = data.value(0);

    return QJsonDocument::fromJson(payload.toString().toUtf8()).object();
}

QString eventMessage(const QJsonObject& payload)
{
    return payload.value(QStringLiteral("message")).toString();
}

QString eventError(const QJsonObject& payload)
{
    return payload.value(QStringLiteral("error")).toString();
}

}

// Constructor for the StorageNode class.
// Captures the LogosAPI instance.
StorageNode::StorageNode(LogosAPI* logosAPI, QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
{
}

void StorageNode::start()
{
    LogosModules logos(m_logosAPI);
    QPointer<StorageNode> self(this);

    if (!logos.modules_state.on("module_state_changed", [self](const QVariantList& data) {
            // Arguments come from https://github.com/logos-co/logos-modules-state-module#contract
            const QString module = data.value(0).toString();

            if (!self || module != QStringLiteral("storage_module")) {
                return;
            }

            const QString newState = data.value(4).toString();

            self->setStorageReady(newState == QStringLiteral("ready"));
        })) {
        qWarning() << "StorageNode: failed to subscribe to module_state_changed events";
    }

    if (logos.modules_state.is_ready(QStringLiteral("storage_module"))) {
        setStorageReady(true);
    }
}

void StorageNode::setStorageReady(bool ready)
{
    if (!ready) {
        m_initialised = false;
        return;
    }

    if (m_initialised) {
        return;
    }

    subscribeToStorageEvents();

    QPointer<StorageNode> self(this);

    m_logosAPI->getClient(QStringLiteral("storage_module"))
        ->whenObjectAvailable(QStringLiteral("storage_module"), [self](bool ready) {
            if (self && ready) {
                self->startNode();
            }
        });
}

void StorageNode::startNode()
{
    if (m_initialised) {
        return;
    }

    LogosModules logos(m_logosAPI);

    const QString configJson =
        QString::fromUtf8(QJsonDocument(sharedUserNodeConfig()).toJson(QJsonDocument::Compact));

    const LogosResult migrated = logos.storage_module.migrateConfig(configJson);

    if (!migrated.success) {
        qWarning() << "StorageNode: failed to migrate the node configuration:"
                   << migrated.getError();
        return;
    }

    if (!logos.storage_module.init(migrated.getValue<QString>())) {
        qWarning() << "StorageNode: failed to initialise storage_module -- packages "
                      "cannot be downloaded over Logos Storage.";
        return;
    }

    m_initialised = true;

    if (!logos.storage_module.start()) {
        qWarning() << "StorageNode: storage_module refused the start command.";
    }
}

// Should be called once the storage_module is initialised.
void StorageNode::subscribeToStorageEvents()
{
    if (m_subscribed) {
        return;
    }

    m_subscribed = true;

    LogosModules logos(m_logosAPI);

    if (!logos.storage_module.on("storageStart", [](const QVariantList& data) {
            const QJsonObject payload = eventPayload(data);

            if (payload.value(QStringLiteral("success")).toBool()) {
                qInfo() << "Storage node started.";
            } else {
                qWarning() << "Storage node failed to start:" << eventMessage(payload);
            }
        })) {
        qWarning() << "StorageNode: failed to subscribe to storageStart events";
    }

    // Guard the subscription in case the storage_module destructs before the event fires.
    QPointer<StorageNode> self(this);

    if (!logos.storage_module.on("storageStop", [self](const QVariantList& data) {
            if (!self) {
                return;
            }

            const QJsonObject payload = eventPayload(data);

            if (payload.value(QStringLiteral("success")).toBool()) {
                self->m_nodeStopped = true;
            } else {
                qWarning() << "Storage node failed to stop:" << eventMessage(payload);
            }

            emit self->stopped();
        })) {
        qWarning() << "StorageNode: failed to subscribe to storageStop events";
    }

    if (!logos.storage_module.on("storageDownloadManifestDone",
                                 [](const QVariantList& data) {
            const QJsonObject payload = eventPayload(data);
            const QString cid = payload.value(QStringLiteral("cid")).toString();

            if (!payload.value(QStringLiteral("success")).toBool()) {
                qInfo() << "Storage: no manifest for" << cid << "--" << eventError(payload);
                return;
            }

            const QJsonObject manifest =
                payload.value(QStringLiteral("manifest")).toObject();

            qInfo() << "Storage: fetching"
                    << manifest.value(QStringLiteral("filename")).toString()
                    << manifest.value(QStringLiteral("datasetSize")).toInteger()
                    << "bytes, cid" << cid;
        })) {
        qWarning() << "StorageNode: failed to subscribe to storageDownloadManifestDone events";
    }

    if (!logos.storage_module.on("storageDownloadDone", [](const QVariantList& data) {
            const QJsonObject payload = eventPayload(data);
            const QString cid = payload.value(QStringLiteral("sessionId")).toString();

            if (payload.value(QStringLiteral("success")).toBool()) {
                qInfo() << "Storage: downloaded" << cid;
            } else {
                qInfo() << "Storage: download of" << cid << "failed --" << eventError(payload);
            }
        })) {
        qWarning() << "StorageNode: failed to subscribe to storageDownloadDone events";
    }
}

void StorageNode::shutdown()
{
    if (!m_initialised) {
        return;
    }

    LogosModules logos(m_logosAPI);

    m_nodeStopped = false;

    const LogosResult stopResult = logos.storage_module.stop();

    if (!stopResult.success) {
        qWarning() << "StorageNode: failed to stop the node:" << stopResult.getError();
        return;
    }

    if (!m_nodeStopped) {
        QEventLoop loop;
        connect(this, &StorageNode::stopped, &loop, &QEventLoop::quit);
        QTimer::singleShot(kStopTimeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
    }

    if (!m_nodeStopped) {
        // If the node cannot be stopped, leave the storage context alone, we should
        // not try to destroy it, maybe something is blocking from UI.
        qWarning() << "StorageNode: the node did not confirm it stopped -- leaving "
                      "the storage context alone.";
        return;
    }

    const LogosResult destroyResult = logos.storage_module.destroy();

    if (!destroyResult.success) {
        qWarning() << "StorageNode: failed to destroy the storage context:"
                   << destroyResult.getError();
        return;
    }

    m_initialised = false;

    qInfo() << "Storage node stopped.";
}
