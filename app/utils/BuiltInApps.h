#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Basecamp's own apps: they come with Basecamp, update only with it, and run
// only from its bundle. Nothing installs under their names (InstallTrust's
// asReplacement()), and a copy found where the user installs is moved aside
// before anything scans there, so the bundled one is the one that runs.
namespace logos::builtin {

// The apps, each with the backend it registers as `<app>_backend`.
inline QStringList apps()
{
    return {QStringLiteral("package_manager_ui")};
}

// Every name a built-in app runs under: the app and its backend.
inline QStringList names()
{
    QStringList all;
    for (const QString& app : apps())
        all << app << app + QStringLiteral("_backend");
    return all;
}

// Whether `name` is one of names(), compared as a case-insensitive file
// system would.
inline bool isBuiltIn(const QString& name)
{
    return names().contains(name, Qt::CaseInsensitive);
}

} // namespace logos::builtin
