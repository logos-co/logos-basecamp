#ifndef LOGOS_BASECAMP_PEERING_SETTINGS_H
#define LOGOS_BASECAMP_PEERING_SETTINGS_H

#include <QJsonObject>
#include <QString>

#include <optional>
#include <string>

namespace LogosBasecamp {

// Settings -> Peering, kept in <user dir>/peering.json:
//   {"enabled": true, "name": "...", "control": {...}, ...}
// "enabled" is Basecamp's; the rest is peering_module's configuration
// (logos-peering docs/api.md), passed to the runtime as it starts.
struct PeeringSettings {
    bool enabled = false;
    QJsonObject config;
    QString error; // the file exists but could not be read
};

QString peeringSettingsPath(const QString& userDir);
PeeringSettings loadPeeringSettings(const QString& path);

// The runtime's peering_config, named `defaultName` unless the file names it;
// nullopt while peering is off.
std::optional<std::string> peeringRuntimeConfig(const PeeringSettings& settings,
                                                const QString& defaultName);

// Turns peering on or off from the next start, keeping the rest of the file.
bool setPeeringEnabled(const QString& path, bool enabled, QString* error = nullptr);

// "Basecamp on <host>", within the 64 characters a peering name may have.
QString defaultPeeringName(const QString& hostName);

// Where a logosctl daemon on this machine keeps its local invite:
// <config dir>/peering/local-invite, the config dir being $LOGOSCTL_CONFIG_DIR
// or ~/.logosctl.
QString daemonLocalInvitePath(const QString& configDirEnv, const QString& homeDir);

} // namespace LogosBasecamp

#endif // LOGOS_BASECAMP_PEERING_SETTINGS_H
