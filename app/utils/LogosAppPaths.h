#pragma once

#include <QCoreApplication>
#include <QStandardPaths>
#include <QString>

namespace LogosAppPaths {

constexpr bool isPortableBuild()
{
#ifdef LOGOS_PORTABLE_BUILD
    return true;
#else
    return false;
#endif
}

inline QString portablePluginsDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/plugins";
}

inline QString portableModulesDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/modules";
}

inline QString nonPortablePluginsDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "Nix" + "/plugins";
}

inline QString nonPortableModulesDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "Nix" + "/modules";
}

inline QString pluginsDirectory()
{
    return isPortableBuild() ? portablePluginsDirectory() : nonPortablePluginsDirectory();
}

inline QString modulesDirectory()
{
    return isPortableBuild() ? portableModulesDirectory() : nonPortableModulesDirectory();
}

} // namespace LogosAppPaths
