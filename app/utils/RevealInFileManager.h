#ifndef LOGOS_BASECAMP_REVEAL_IN_FILE_MANAGER_H
#define LOGOS_BASECAMP_REVEAL_IN_FILE_MANAGER_H

#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

namespace LogosBasecamp {

enum class FileManagerPlatform { MacOS, Windows, Other };

FileManagerPlatform currentFileManagerPlatform();

// What shows `dir` in the file manager: a program to start detached, or, when
// `program` is empty, a URL for the desktop to open.
struct FileManagerLaunch {
    QString program;
    QStringList arguments;
    QUrl url;
};

// Finder and Explorer show `dir` selected in its parent: opened, Finder's
// column view lands on its first entry (keyring) instead.
FileManagerLaunch fileManagerLaunchFor(FileManagerPlatform platform, const QString& dir);

struct FileManagerLauncher {
    std::function<bool(const QString& program, const QStringList& arguments)> startDetached;
    std::function<bool(const QUrl& url)> openUrl;
};

// QProcess::startDetached and QDesktopServices::openUrl.
FileManagerLauncher systemFileManagerLauncher();

// Creates `dir` if missing, then shows it; false if either fails.
bool revealInFileManager(const QString& dir, FileManagerPlatform platform,
                         const FileManagerLauncher& launcher);

} // namespace LogosBasecamp

#endif
