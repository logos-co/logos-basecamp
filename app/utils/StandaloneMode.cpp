#include "utils/StandaloneMode.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace LogosBasecamp {

namespace {

struct Manifest {
    QString name;
    QString type;
};

bool readManifest(const QString& moduleDir, Manifest& out)
{
    QFile file(QDir(moduleDir).filePath(QStringLiteral("manifest.json")));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    out.name = obj.value(QStringLiteral("name")).toString();
    out.type = obj.value(QStringLiteral("type")).toString();
    return !out.name.isEmpty();
}

bool isUiType(const QString& type)
{
    return type.startsWith(QStringLiteral("ui"));
}

void appendUnique(QStringList& list, const QString& value)
{
    if (!list.contains(value)) list << value;
}

// Scan dirs: a later dir wins a name clash, so re-adding one moves it last.
void appendLast(QStringList& list, const QString& value)
{
    list.removeAll(value);
    list << value;
}

// UI apps installed directly under `dir`.
QStringList uiModulesIn(const QString& dir)
{
    QStringList names;
    const auto entries = QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& entry : entries) {
        Manifest m;
        if (readManifest(entry.absoluteFilePath(), m) && isUiType(m.type))
            names << m.name;
    }
    return names;
}

// An install root has modules/ and/or plugins/ (lgpm's layout). Returns false
// if `dir` is not one.
bool addInstallRoot(HostProfile& profile, const QString& dir, QStringList* uiModules)
{
    const QString modules = QDir(dir).filePath(QStringLiteral("modules"));
    const QString plugins = QDir(dir).filePath(QStringLiteral("plugins"));
    const bool hasModules = QFileInfo(modules).isDir();
    const bool hasPlugins = QFileInfo(plugins).isDir();
    if (!hasModules && !hasPlugins) return false;

    if (hasModules) appendLast(profile.coreModuleRoots, modules);
    if (hasPlugins) {
        appendLast(profile.uiPluginRoots, plugins);
        if (uiModules) *uiModules << uiModulesIn(plugins);
    }
    return true;
}

QString absoluteDir(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

StandaloneResolution addStandaloneModule(HostProfile& profile, const QString& path)
{
    const QString dir = absoluteDir(path);
    if (!QFileInfo(dir).isDir())
        return {false, QStringLiteral("--module: not a directory: %1").arg(dir)};

    Manifest m;
    if (readManifest(dir, m)) {
        return {false, QStringLiteral("--module: %1 is a single module; pass the install "
                                      "root that holds it (with modules/ or plugins/), e.g. "
                                      "the output of `nix build .#install`, so the modules "
                                      "next to it are not exposed").arg(dir)};
    }

    QStringList uiModules;
    if (!addInstallRoot(profile, dir, &uiModules)) {
        return {false, QStringLiteral("--module: %1 has no modules/ or plugins/ — pass the "
                                      "output of `nix build .#install`").arg(dir)};
    }
    for (const QString& name : uiModules) appendUnique(profile.appsToOpen, name);
    return {};
}

StandaloneResolution addStandaloneModulesDir(HostProfile& profile, const QString& path)
{
    const QString dir = absoluteDir(path);
    if (!QFileInfo(dir).isDir())
        return {false, QStringLiteral("--modules-dir: not a directory: %1").arg(dir)};

    if (!addInstallRoot(profile, dir, nullptr)) {
        appendLast(profile.coreModuleRoots, dir);
        appendLast(profile.uiPluginRoots, dir);
    }
    return {};
}

void applyStandaloneCapabilities(HostProfile& profile)
{
    profile.launcherLimitedToOpenedApps = true;
    profile.useUserModules = false;
    profile.packageCatalog = false;
    profile.repositoryManagement = false;
    profile.registerUrlScheme = false;
    profile.singleInstance = false;
    profile.defaultUserDir = QDir::current().absoluteFilePath(QStringLiteral(".logos-basecamp-dev"));
    profile.applicationName = QStringLiteral("LogosBasecampStandalone");
    profile.devHost = true;
}

StandaloneResolution addStandaloneQmlSource(HostProfile& profile, const QString& arg)
{
    const int eq = arg.indexOf(QLatin1Char('='));
    if (eq <= 0 || eq == arg.size() - 1)
        return {false, QStringLiteral("--qml-source: expected <app>=<dir>, got '%1'").arg(arg)};

    const QString dir = absoluteDir(arg.mid(eq + 1));
    if (!QFileInfo(dir).isDir())
        return {false, QStringLiteral("--qml-source: not a directory: %1").arg(dir)};

    profile.qmlSources.insert(arg.left(eq), dir);
    return {};
}

} // namespace LogosBasecamp
