#include "PeeringController.h"

#include "CoreModuleManager.h"
#include "utils/PeeringSettings.h"

#include "logos_api.h"
#include "logos_api_client.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

namespace {

const QString kPeering = QStringLiteral("peering_module");

// peering_module answers JSON objects; the Qt client may hand them over as a
// map, a QJsonObject or JSON text.
QVariantMap asMap(const QVariant& value)
{
    if (value.metaType() == QMetaType::fromType<QJsonObject>())
        return value.toJsonObject().toVariantMap();
    if (value.metaType() == QMetaType::fromType<QJsonValue>())
        return value.toJsonValue().toObject().toVariantMap();
    if (value.metaType() == QMetaType::fromType<QString>()) {
        const QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8());
        return doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
    }
    return value.toMap();
}

QString errorOf(const QVariantMap& reply)
{
    return reply.value(QStringLiteral("error")).toString();
}

} // namespace

PeeringController::PeeringController(LogosAPI* logosAPI, CoreModuleManager* core,
                                     QString settingsPath, QString localInvitePath, QObject* parent)
    : QObject(parent)
    , m_logosAPI(logosAPI)
    , m_core(core)
    , m_settingsPath(std::move(settingsPath))
    , m_localInvitePath(std::move(localInvitePath))
{
    const auto settings = LogosBasecamp::loadPeeringSettings(m_settingsPath);
    m_enabledAtStart = settings.enabled && settings.error.isEmpty();
    m_enabled = m_enabledAtStart;
    m_error = settings.error;
    if (m_core)
        connect(m_core, &CoreModuleManager::coreModulesChanged, this, [this]() {
            if (running() && !m_subscribed) refresh();
        });
}

bool PeeringController::running() const
{
    return m_enabledAtStart && m_logosAPI && m_core && m_core->loadedModules().contains(kPeering);
}

QVariantMap PeeringController::state() const
{
    QVariantList imports;
    for (auto it = m_imports.constBegin(); it != m_imports.constEnd(); ++it) {
        QVariantMap item = it.value().toMap();
        const QVariantMap live = m_importStates.value(it.key()).toMap();
        item.insert(QStringLiteral("name"), it.key());
        item.insert(QStringLiteral("state"), live.value(QStringLiteral("state"), QStringLiteral("configured")));
        item.insert(QStringLiteral("reason"), live.value(QStringLiteral("reason")));
        imports.append(item);
    }
    return {
        {QStringLiteral("enabled"), m_enabled},
        {QStringLiteral("restartRequired"), m_enabled != m_enabledAtStart},
        {QStringLiteral("running"), running()},
        {QStringLiteral("error"), m_error},
        {QStringLiteral("status"), m_status},
        {QStringLiteral("peers"), m_peers},
        {QStringLiteral("pending"), m_pending},
        {QStringLiteral("imports"), imports},
        {QStringLiteral("localInvitePath"), m_localInvitePath},
        {QStringLiteral("localInviteFound"), QFileInfo::exists(m_localInvitePath)},
    };
}

void PeeringController::call(const QString& method, const QVariantList& args, Done done)
{
    LogosAPIClient* client = running() ? m_logosAPI->getClient(kPeering) : nullptr;
    if (!client) {
        done({{QStringLiteral("error"), tr("Peering is not running.")}});
        return;
    }
    QPointer<PeeringController> self(this);
    client->invokeRemoteMethodAsync(kPeering, method, args, [self, done](QVariant result) {
        if (!self) return;
        const QVariantMap reply = asMap(result);
        done(reply.isEmpty() && !result.isValid()
                 ? QVariantMap{{QStringLiteral("error"), tr("peering_module did not answer.")}}
                 : reply);
    });
}

void PeeringController::subscribe()
{
    LogosAPIClient* client = running() ? m_logosAPI->getClient(kPeering) : nullptr;
    if (m_subscribed || !client) return;
    m_subscribed = true;
    QPointer<PeeringController> self(this);
    for (const char* event : {"peersChanged", "pairingRequested", "importsChanged",
                              "importStateChanged", "exportsChanged"})
        client->onEventWhenAvailable(kPeering, QString::fromLatin1(event),
                                     [self](const QString&, const QVariantList&) {
                                         if (self) self->refresh();
                                     });
}

void PeeringController::refresh()
{
    if (!running()) {
        emit stateChanged();
        return;
    }
    subscribe();
    QPointer<PeeringController> self(this);
    call(QStringLiteral("status"), {}, [self](const QVariantMap& reply) {
        if (!self) return;
        self->m_error = errorOf(reply);
        if (self->m_error.isEmpty()) self->m_status = reply;
        emit self->stateChanged();
    });
    call(QStringLiteral("peers"), {}, [self](const QVariantMap& reply) {
        if (!self || !errorOf(reply).isEmpty()) return;
        self->m_peers = reply.value(QStringLiteral("peers")).toList();
        emit self->stateChanged();
    });
    call(QStringLiteral("pending"), {}, [self](const QVariantMap& reply) {
        if (!self || !errorOf(reply).isEmpty()) return;
        self->m_pending = reply.value(QStringLiteral("pending")).toList();
        emit self->stateChanged();
    });
    call(QStringLiteral("imports"), {}, [self](const QVariantMap& reply) {
        if (!self || !errorOf(reply).isEmpty()) return;
        self->m_imports = reply;
        emit self->stateChanged();
    });
    call(QStringLiteral("importStates"), {}, [self](const QVariantMap& reply) {
        if (!self || !errorOf(reply).isEmpty()) return;
        self->m_importStates = reply;
        emit self->stateChanged();
    });
}

void PeeringController::manage(const QString& operation, const QString& method,
                               const QVariantList& args)
{
    QPointer<PeeringController> self(this);
    call(method, args, [self, operation](const QVariantMap& reply) {
        if (!self) return;
        const QString error = errorOf(reply);
        emit self->operationCompleted(operation, error.isEmpty(), error);
        self->refresh();
    });
}

void PeeringController::setEnabled(bool enabled)
{
    QString error;
    if (!LogosBasecamp::setPeeringEnabled(m_settingsPath, enabled, &error)) {
        emit operationCompleted(QStringLiteral("enable"), false, error);
        return;
    }
    m_enabled = enabled;
    emit operationCompleted(QStringLiteral("enable"), true, QString());
    emit stateChanged();
}

void PeeringController::linkLocalDaemon(const QString& invitePath)
{
    const QString path = invitePath.isEmpty() ? m_localInvitePath : invitePath;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit operationCompleted(QStringLiteral("link"), false,
                                tr("No daemon invite at %1: %2").arg(path, file.errorString()));
        return;
    }
    // One line, never logged: the invite's secret pairs whoever holds it.
    const QString invite = QString::fromUtf8(file.readLine(4096)).trimmed();
    manage(QStringLiteral("link"), QStringLiteral("redeemInvite"), {invite});
}

void PeeringController::pairWith(const QString& host, int port)
{
    manage(QStringLiteral("pair"), QStringLiteral("pairWith"), {host.trimmed(), port});
}

void PeeringController::confirmPairing(const QString& id)
{
    manage(QStringLiteral("confirm"), QStringLiteral("confirmPairing"), {id});
}

void PeeringController::rejectPairing(const QString& id)
{
    manage(QStringLiteral("reject"), QStringLiteral("rejectPairing"), {id});
}

void PeeringController::removePeer(const QString& peer)
{
    manage(QStringLiteral("remove"), QStringLiteral("removePeer"), {peer});
}

void PeeringController::fetchPeerExports(const QString& peer)
{
    QPointer<PeeringController> self(this);
    call(QStringLiteral("peerExports"), {peer}, [self, peer](const QVariantMap& reply) {
        if (!self) return;
        const QString error = errorOf(reply);
        if (!error.isEmpty()) {
            emit self->operationCompleted(QStringLiteral("exports"), false, error);
            return;
        }
        QVariantList exports;
        const QVariantMap offered = reply.value(QStringLiteral("exports")).toMap();
        for (auto it = offered.constBegin(); it != offered.constEnd(); ++it) {
            QVariantMap item = it.value().toMap();
            item.insert(QStringLiteral("module"), it.key());
            exports.append(item);
        }
        emit self->peerExportsFetched(peer, exports);
    });
}

void PeeringController::importModule(const QString& peer, const QString& module, bool events)
{
    const QVariantMap rule{{QStringLiteral("from"), peer},
                           {QStringLiteral("module"), module},
                           {QStringLiteral("allowed_callers"), QVariantList{QStringLiteral("*")}},
                           {QStringLiteral("events"), events}};
    manage(QStringLiteral("import"), QStringLiteral("setImport"), {module, rule});
}

void PeeringController::removeImport(const QString& name)
{
    manage(QStringLiteral("unimport"), QStringLiteral("removeImport"), {name});
}
