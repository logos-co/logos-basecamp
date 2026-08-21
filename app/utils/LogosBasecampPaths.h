#pragma once

#include <QCoreApplication>
#include <QStandardPaths>
#include <QString>
#include <QDir>
#include <QProcessEnvironment>
#include <QRegularExpression>

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

// ── Session log files ────────────────────────────────────────────────────────
// The one place that knows how LogRedirector names its files. The writer
// (app/utils/LogRedirector) and the readers (main_ui's Settings → Logs,
// retention) both go through these, so the pattern is never spelled out
// twice. A session is basecamp_<stamp>.log followed by rotations
// basecamp_<stamp>.001.log, .002.log, … (three digits minimum, more when a
// long session needs them).

inline constexpr const char* kSessionLogStampFormat = "yyyyMMdd_HHmmss";
inline constexpr const char* kSessionLogGlob = "basecamp_*.log";

// Set by main.cpp to the running session's first file so the UI plugin,
// which is loaded rather than linked, can tell which file is being written.
inline constexpr const char* kSessionLogEnvVar = "LOGOS_SESSION_LOG";
// Retention at launch: oldest sessions go until both limits hold. A single
// chatty session can run to gigabytes while a quiet one is a few KB, so a
// session count alone is not enough. 0 disables either limit.
inline constexpr const char* kKeepSessionsEnvVar = "LOGOS_LOG_KEEP_SESSIONS";
inline constexpr int kDefaultKeepSessions = 30;
inline constexpr const char* kMaxLogMbEnvVar = "LOGOS_LOG_MAX_MB";
inline constexpr int kDefaultMaxLogMb = 1024;

inline QString sessionLogFileName(const QString& stamp, int rotation)
{
    if (rotation <= 0)
        return QStringLiteral("basecamp_%1.log").arg(stamp);
    return QStringLiteral("basecamp_%1.%2.log").arg(stamp).arg(rotation, 3, 10, QChar('0'));
}

// Decompose a file name produced by sessionLogFileName(). Returns false for
// anything else found in the directory.
inline bool parseSessionLogFileName(const QString& fileName, QString* stamp, int* rotation)
{
    static const QRegularExpression re(
        QStringLiteral("^basecamp_(\\d{8}_\\d{6})(?:\\.(\\d{3,}))?\\.log$"));
    const QRegularExpressionMatch m = re.match(fileName);
    if (!m.hasMatch()) return false;
    if (stamp) *stamp = m.captured(1);
    if (rotation) *rotation = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
    return true;
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
