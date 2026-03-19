#pragma once

#include <QCoreApplication>
#include <QStandardPaths>
#include <QString>
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

inline QString portablePluginsDirectory()
{
    return dataDirectory() + "/plugins";
}

inline QString portableModulesDirectory()
{
    return dataDirectory() + "/modules";
}

inline QString nonPortablePluginsDirectory()
{
    return dataDirectory() + "Dev" + "/plugins";
}

inline QString nonPortableModulesDirectory()
{
    return dataDirectory() + "Dev" + "/modules";
}

inline QString pluginsDirectory()
{
    return isPortableBuild() ? portablePluginsDirectory() : nonPortablePluginsDirectory();
}

inline QString modulesDirectory()
{
    return isPortableBuild() ? portableModulesDirectory() : nonPortableModulesDirectory();
}

} // namespace LogosBasecampPaths
