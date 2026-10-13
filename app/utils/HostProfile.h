#ifndef LOGOS_BASECAMP_HOST_PROFILE_H
#define LOGOS_BASECAMP_HOST_PROFILE_H

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace LogosBasecamp {

// What this process hosts and offers. Built once in main(), passed down;
// components read capabilities, never the kind of host. Defaults are the
// desktop app.
struct HostProfile {
    QStringList coreModuleRoots;
    QStringList uiPluginRoots;

    QStringList appsToOpen;
    // App name -> source dir its view is loaded from and rebuilt on save
    // (`--qml-source`).
    QHash<QString, QString> qmlSources;
    bool launcherLimitedToOpenedApps = false;

    bool useUserModules = true;        // user dir's modules/ and plugins/
    bool packageCatalog = true;        // downloader, App Manager, Package Manager
    bool repositoryManagement = true;  // Settings → Package Repositories
    bool registerUrlScheme = true;     // basecamp://
    bool singleInstance = true;        // a second launch hands over to this one

    QString defaultUserDir;   // without --user-dir / LOGOS_USER_DIR; empty = platform default
    QString applicationName;  // keys QSettings; empty = LogosBasecamp

    bool devHost = false;     // labelled as a developer's host

    // false: a dependency outside its declared version range loads with a
    // warning instead of being refused. A missing dependency still refuses.
    bool enforceDependencyVersions = true;

    QList<int> availableSections() const;  // ShellSection values

    // package_manager_ui is the Package Manager section — shell furniture, not
    // an app — wherever that section is offered: always in the desktop app,
    // and in standalone mode when it is one of the apps opened (`--module`).
    // Elsewhere (loaded only as a dependency) it is docked like any app.
    bool packageManagerUiIsSection() const;
};

} // namespace LogosBasecamp

#endif // LOGOS_BASECAMP_HOST_PROFILE_H
