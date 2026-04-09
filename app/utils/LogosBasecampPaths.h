#pragma once

#include <QCoreApplication>
#include <QStandardPaths>
#include <QString>
#include <QDir>
#include <QProcessEnvironment>

namespace LogosBasecampPaths {

constexpr bool isPortableBuild()
{
#ifdef LOGOS_PORTABLE_BUILD
    return true;
#else
    return false;
#endif
}

// Base data directory. LOGOS_DATA_DIR overrides QStandardPaths so tests and
// CI can redirect writes to a writable path (e.g. the Nix build output dir)
// without relying on the system home directory.
inline QString dataDirectory()
{
    const QString override = qEnvironmentVariable("LOGOS_DATA_DIR");
    if (!override.isEmpty())
        return override;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

// Portable vs non-portable base: portable uses dataDirectory(),
// non-portable appends "Dev" (e.g. for side-by-side dev installs).
inline QString portableBaseDirectory()
{
    return dataDirectory();
}

inline QString nonPortableBaseDirectory()
{
    return dataDirectory() + "Dev";
}

inline QString baseDirectory()
{
    return isPortableBuild() ? portableBaseDirectory() : nonPortableBaseDirectory();
}

// Plugin and module install directories
inline QString pluginsDirectory()
{
    return baseDirectory() + "/plugins";
}

inline QString modulesDirectory()
{
    return baseDirectory() + "/modules";
}

// Persistence directories for module instance state.
// Core modules (process-isolated) persist under module_data/.
inline QString moduleDataDirectory()
{
    return baseDirectory() + "/module_data";
}

// Embedded directories — read-only, pre-installed at build time alongside the binary.
inline QString embeddedModulesDirectory()
{
    QDir appDir(QCoreApplication::applicationDirPath());
    appDir.cdUp();
    return QDir::cleanPath(appDir.absolutePath() + "/modules");
}

inline QString embeddedPluginsDirectory()
{
    QDir appDir(QCoreApplication::applicationDirPath());
    appDir.cdUp();
    return QDir::cleanPath(appDir.absolutePath() + "/plugins");
}

} // namespace LogosBasecampPaths
