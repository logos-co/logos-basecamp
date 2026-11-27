#pragma once

#include "DefaultApps.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>

class ICoreRuntime;

// Basecamp's own apps: they come with Basecamp, update only with it, and run
// only from its bundle. Nothing installs under their names (InstallTrust's
// asReplacement()), and a copy found where the user installs is moved aside
// before anything scans there, so the bundled one is the one that runs.
namespace logos::builtin {

// The apps, each with the backend it registers as `<app>_backend`: the
// default-app table's.
inline QStringList apps()
{
    return logos::defaultapps::apps();
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

// A copy of a built-in app found where the user installs. Found by asking the
// scans that pick what runs, never by reading manifests here, so it is found
// exactly as they find it.
struct SetAside {
    QString name;          // the name it runs under
    QString from;          // its package directory
    QString to;            // where it is now; empty if it was not moved
    bool movable = true;   // directly in the user's dir; anything else stays put
    // The core had that name loaded: a rescan repoints a running module's
    // entry at the copy it found, so what runs may be the copy or Basecamp's
    // own. Either way it is stopped before the copy moves.
    bool loaded = false;
};

// The built-in names the core knows from under `userRoot`, by its
// modulesInfoJson().
QList<SetAside> copiesInCore(const QString& modulesInfoJson, const QString& userRoot);

// The built-in apps package_manager lists from anywhere but Basecamp itself,
// by its getInstalledUiPlugins(); movable when directly in `userRoot`.
QList<SetAside> copiesInApps(const QVariantList& uiPlugins, const QString& userRoot);

// Moves `copy.from` into a new directory under `setAsideRoot`, which nothing
// scans, and says where in `to`.
SetAside setAside(SetAside copy, const QString& setAsideRoot);

// copiesInCore() of `core`, each unloaded with its dependents when loaded and
// then set aside, then a rescan if any moved so the core forgets them. One
// left behind is refused when it would load (InstalledPlacement's preflight()).
QList<SetAside> setAsideFromCore(ICoreRuntime& core, const QString& userRoot,
                                 const QString& setAsideRoot);

// What the user is told about one.
QString setAsideTitle(const SetAside& copy);
QString setAsideMessage(const SetAside& copy);

} // namespace logos::builtin
