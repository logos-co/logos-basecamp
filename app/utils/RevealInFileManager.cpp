#include "RevealInFileManager.h"

#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QProcess>

namespace LogosBasecamp {

FileManagerPlatform currentFileManagerPlatform()
{
#if defined(Q_OS_MACOS)
    return FileManagerPlatform::MacOS;
#elif defined(Q_OS_WIN)
    return FileManagerPlatform::Windows;
#else
    return FileManagerPlatform::Other;
#endif
}

FileManagerLaunch fileManagerLaunchFor(FileManagerPlatform platform, const QString& dir)
{
    switch (platform) {
    case FileManagerPlatform::MacOS:
        return {QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), dir}, {}};
    case FileManagerPlatform::Windows:
        // Not QDir::toNativeSeparators: that is a no-op off Windows.
        return {QStringLiteral("explorer.exe"),
                {QStringLiteral("/select,"), QString(dir).replace(QLatin1Char('/'), QLatin1Char('\\'))},
                {}};
    case FileManagerPlatform::Other:
        break;
    }
    return {{}, {}, QUrl::fromLocalFile(dir)};
}

FileManagerLauncher systemFileManagerLauncher()
{
    return {
        [](const QString& program, const QStringList& arguments) {
            return QProcess::startDetached(program, arguments);
        },
        [](const QUrl& url) { return QDesktopServices::openUrl(url); },
    };
}

bool revealInFileManager(const QString& dir, FileManagerPlatform platform,
                         const FileManagerLauncher& launcher)
{
    if (!QDir().mkpath(dir)) {
        qWarning() << "Couldn't create the user directory" << dir;
        return false;
    }
    const FileManagerLaunch launch = fileManagerLaunchFor(platform, dir);
    const bool opened = launch.program.isEmpty()
        ? launcher.openUrl(launch.url)
        : launcher.startDetached(launch.program, launch.arguments);
    if (!opened) {
        qWarning() << "Couldn't open the user directory" << dir;
        return false;
    }
    return true;
}

} // namespace LogosBasecamp
