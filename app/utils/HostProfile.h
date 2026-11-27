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

    // App backends and core modules ask and provide through intents_module, with
    // Basecamp as its presenter; apps' screens keep the QML path beside it until
    // 0.5. False only when the module failed to load.
    bool intentsModule = true;
    // How intents_module reaches the person: "bundled" opens a request from
    // what ships with Basecamp at once and any other only after a click in its
    // app's screen; "interactive" always waits for that click; "immediate"
    // opens every request, in developer builds only. What doesn't open waits
    // in the pending list. See intentsAttentionFor().
    QString intentsAttention = QStringLiteral("bundled");

    // false: a dependency outside its declared version range loads with a
    // warning instead of being refused. A missing dependency still refuses.
    bool enforceDependencyVersions = true;

    QList<int> availableSections() const;  // ShellSection values

    // The section `app` is shown as — shell furniture, not an app — when it
    // is a default app with a section (DefaultApps.h) and that section is
    // offered: always in the desktop app, and in standalone mode when it is
    // one of the apps opened (`--module`). -1 otherwise: loaded only as a
    // dependency, it is docked like any app.
    int sectionOfApp(const QString& app) const;
    bool isSectionApp(const QString& app) const { return sectionOfApp(app) >= 0; }
};

// The intents attention `requested` (--intents-attention or
// LOGOS_INTENTS_ATTENTION) asks for: "bundled" when it is empty, unknown, or
// "immediate" outside a `developerBuild`, with `warning` saying so.
QString intentsAttentionFor(const QString& requested, bool developerBuild, QString* warning);

} // namespace LogosBasecamp

#endif // LOGOS_BASECAMP_HOST_PROFILE_H
