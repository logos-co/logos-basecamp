#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class CoreModuleManager;
class LogosAPI;

// Settings -> Peering: links between this runtime and other Logos runtimes,
// through peering_module (logos-peering docs/api.md). Calls go as the shell,
// asynchronously; peering_module's events trigger a refresh.
class PeeringController : public QObject {
    Q_OBJECT

public:
    PeeringController(LogosAPI* logosAPI, CoreModuleManager* core, QString settingsPath,
                      QString localInvitePath, QObject* parent = nullptr);

    // What the Peering view shows: enabled, restartRequired, running, error,
    // status, peers, pending, imports and the local invite's path and presence.
    QVariantMap state() const;

    void refresh();
    void setEnabled(bool enabled);
    void linkLocalDaemon(const QString& invitePath);
    void pairWith(const QString& host, int port);
    // Lets runtimes that are not paired yet ask to pair, for `seconds` (0 closes).
    void openPairingWindow(int seconds);
    void confirmPairing(const QString& id);
    void rejectPairing(const QString& id);
    void removePeer(const QString& peer);
    void fetchPeerExports(const QString& peer);
    // Imports `module` from `peer` under the same name, for any local caller.
    void importModule(const QString& peer, const QString& module, bool events);
    void removeImport(const QString& name);

signals:
    void stateChanged();
    void operationCompleted(const QString& operation, bool ok, const QString& message);
    void peerExportsFetched(const QString& peer, const QVariantList& exports);
    // Another runtime asks to pair and waits for this side to compare codes:
    // {id, code, peer_name, peer_display_id, peer_runtime_id, role, expires_ms}.
    void pairingRequested(const QVariantMap& request);

private:
    using Done = std::function<void(const QVariantMap&)>;
    bool running() const;
    void call(const QString& method, const QVariantList& args, Done done);
    // A management call whose outcome the view reports.
    void manage(const QString& operation, const QString& method, const QVariantList& args);
    void subscribe();

    LogosAPI* m_logosAPI;
    CoreModuleManager* m_core;
    QString m_settingsPath;
    QString m_localInvitePath;
    bool m_enabledAtStart = false;
    bool m_enabled = false;
    bool m_subscribed = false;
    QString m_error;
    QVariantMap m_status;
    QVariantList m_peers;
    QVariantList m_pending;
    QVariantMap m_imports;
    QVariantMap m_importStates;
};
