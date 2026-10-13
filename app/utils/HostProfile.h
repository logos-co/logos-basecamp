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

    QList<int> availableSections() const;  // ShellSection values
};

} // namespace LogosBasecamp

#endif // LOGOS_BASECAMP_HOST_PROFILE_H
