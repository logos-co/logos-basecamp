#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

// A fresh package-manager scan is the authority for pre-install removal.
// Embedded packages must remain in the application bundle; installPlugin
// writes their replacement to the user directory instead.
inline bool shouldRemoveBeforeInstall(const QVariantList& installedPackages,
                                      const QString& packageName)
{
    for (const QVariant& value : installedPackages) {
        const QVariantMap package = value.toMap();
        if (package.value(QStringLiteral("name")).toString() == packageName
            || package.value(QStringLiteral("moduleName")).toString() == packageName) {
            return package.value(QStringLiteral("installType")).toString()
                == QLatin1String("user");
        }
    }
    return false;
}
