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

// Base data directory from QStandardPaths. Only consumed by the
// portable/non-portable selection in baseDirectory(); callers that need an
// explicit override (tests, CI, --user-dir) go through LOGOS_USER_DIR instead.
inline QString dataDirectory()
{
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
    // LOGOS_USER_DIR overrides the base directory as-is, bypassing the
    // portable/non-portable selection and the "Dev" suffix. Set by --user-dir
    // so callers get the exact path they asked for.
    const QString baseOverride = qEnvironmentVariable("LOGOS_USER_DIR");
    if (!baseOverride.isEmpty())
        return baseOverride;
    return isPortableBuild() ? portableBaseDirectory() : nonPortableBaseDirectory();
}

// Resolve Basecamp's owning profile once, then make that exact root explicit
// for every child process. Out-of-process UI backends run in `ui-host`, whose
// QStandardPaths::AppDataLocation would otherwise be based on the child
// application name and collapse ordinary Basecamp launches into a shared
// `.../Logos/ui-host` tree.
//
// QProcess inherits the current process environment by default. Calling this
// before any ViewModuleHost can spawn therefore gives default-profile and
// --user-dir launches the same explicit, application-name-independent
// contract. Existing LOGOS_USER_DIR overrides are preserved verbatim.
inline QString resolveAndExportBaseDirectory()
{
    const QString resolved = baseDirectory();
    qputenv("LOGOS_USER_DIR", resolved.toUtf8());
    return resolved;
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

// Directory for app log files (stdout/stderr capture, rotated per session).
inline QString logsDirectory()
{
    return baseDirectory() + "/logs";
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
