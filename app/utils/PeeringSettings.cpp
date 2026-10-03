#include "PeeringSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace LogosBasecamp {

namespace {
constexpr int kMaxNameLength = 64;
const QString kEnabled = QStringLiteral("enabled");
} // namespace

QString peeringSettingsPath(const QString& userDir)
{
    return QDir(userDir).filePath(QStringLiteral("peering.json"));
}

PeeringSettings loadPeeringSettings(const QString& path)
{
    PeeringSettings settings;
    QFile file(path);
    if (!file.exists()) return settings;
    if (!file.open(QIODevice::ReadOnly)) {
        settings.error = QStringLiteral("%1 cannot be read: %2").arg(path, file.errorString());
        return settings;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!doc.isObject()) {
        settings.error = QStringLiteral("%1 is not a JSON object: %2").arg(path, parseError.errorString());
        return settings;
    }
    settings.config = doc.object();
    settings.enabled = settings.config.value(kEnabled).toBool(false);
    settings.config.remove(kEnabled);
    return settings;
}

std::optional<std::string> peeringRuntimeConfig(const PeeringSettings& settings,
                                                const QString& defaultName)
{
    if (!settings.enabled || !settings.error.isEmpty()) return std::nullopt;
    QJsonObject config = settings.config;
    if (!config.contains(QStringLiteral("name")) && !defaultName.isEmpty())
        config.insert(QStringLiteral("name"), defaultName);
    return QJsonDocument(config).toJson(QJsonDocument::Compact).toStdString();
}

bool setPeeringEnabled(const QString& path, bool enabled, QString* error)
{
    PeeringSettings settings = loadPeeringSettings(path);
    if (!settings.error.isEmpty()) {
        if (error) *error = settings.error;
        return false;
    }
    QJsonObject doc = settings.config;
    doc.insert(kEnabled, enabled);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(doc).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        if (error) *error = QStringLiteral("%1 cannot be written: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

QString defaultPeeringName(const QString& hostName)
{
    QString host = hostName.trimmed();
    if (host.endsWith(QStringLiteral(".local"))) host.chop(6);
    const QString name = host.isEmpty() ? QStringLiteral("Basecamp")
                                         : QStringLiteral("Basecamp on %1").arg(host);
    return name.left(kMaxNameLength);
}

QString daemonLocalInvitePath(const QString& configDirEnv, const QString& homeDir)
{
    const QString configDir = !configDirEnv.isEmpty()
        ? configDirEnv : QDir(homeDir).filePath(QStringLiteral(".logosctl"));
    return QDir(configDir).filePath(QStringLiteral("peering/local-invite"));
}

} // namespace LogosBasecamp
