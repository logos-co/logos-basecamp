#include "BuiltInApps.h"

#include "InstalledPlacement.h"
#include "interfaces/ICoreRuntime.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>

namespace logos::builtin {

QList<SetAside> copiesInCore(const QString& modulesInfoJson, const QString& userRoot)
{
    QList<SetAside> copies;
    if (userRoot.isEmpty())
        return copies;
    for (const QJsonValue& v : QJsonDocument::fromJson(modulesInfoJson.toUtf8()).array()) {
        const QJsonObject module = v.toObject();
        const QString name = module.value(QStringLiteral("name")).toString();
        if (!isBuiltIn(name))
            continue;
        const QString path = module.value(QStringLiteral("path")).toString();
        const integrity::Placement where = integrity::placementUnder(path, userRoot);
        if (where.inside)
            copies.append({name, where.packageDir.isEmpty() ? path : where.packageDir, {}, true,
                           module.value(QStringLiteral("loaded")).toBool()});
    }
    return copies;
}

QList<SetAside> copiesInApps(const QVariantList& uiPlugins, const QString& userRoot)
{
    QList<SetAside> copies;
    for (const QVariant& v : uiPlugins) {
        const QVariantMap app = v.toMap();
        const QString name = app.value(QStringLiteral("name")).toString();
        if (!isBuiltIn(name) || app.value(QStringLiteral("installType")).toString() == QLatin1String("embedded"))
            continue;
        // "Loose" in the root is a package dir directly in it.
        const QString dir = app.value(QStringLiteral("installDir")).toString();
        const integrity::Placement where = integrity::placementUnder(dir, userRoot);
        copies.append({name, dir, {}, where.inside && where.packageDir.isEmpty()});
    }
    return copies;
}

SetAside setAside(SetAside copy, const QString& setAsideRoot)
{
    if (!copy.movable || copy.from.isEmpty() || !QDir().mkpath(setAsideRoot))
        return copy;
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = QDir(setAsideRoot).filePath(QFileInfo(QDir::cleanPath(copy.from)).fileName()
                                                     + QLatin1Char('-') + stamp);
    QString to = base;
    for (int n = 2; QFileInfo(to).exists() || QFileInfo(to).isSymLink(); ++n)
        to = base + QLatin1Char('-') + QString::number(n);
    if (QDir().rename(copy.from, to))
        copy.to = to;
    return copy;
}

QList<SetAside> setAsideFromCore(ICoreRuntime& core, const QString& userRoot, const QString& setAsideRoot)
{
    QList<SetAside> copies;
    bool moved = false;
    for (const SetAside& found : copiesInCore(core.modulesInfoJson(), userRoot)) {
        if (found.loaded && !core.unloadModule(found.name, /*withDependents=*/true))
            qWarning().noquote() << "Could not stop" << found.name << "before moving" << found.from;
        copies.append(setAside(found, setAsideRoot));
        moved = moved || !copies.last().to.isEmpty();
    }
    if (moved)
        core.refreshModules();
    return copies;
}

QString setAsideTitle(const SetAside& copy)
{
    return copy.to.isEmpty() ? QStringLiteral("Built-in App Copy Not Used")
                             : QStringLiteral("Built-in App Copy Moved");
}

QString setAsideMessage(const SetAside& copy)
{
    const QString from = QDir::toNativeSeparators(copy.from);
    if (copy.to.isEmpty())
        return QStringLiteral("%1 comes with Basecamp, so only Basecamp's own copy runs. The copy "
                              "in %2 could not be moved, so it will not be started. Remove it, "
                              "then restart Basecamp.")
            .arg(copy.name, from);
    const QString stopped = copy.loaded
        ? QStringLiteral(" Basecamp stopped what was running under that name; reopen Package Manager "
                         "to start its own copy.")
        : QString();
    return QStringLiteral("%1 comes with Basecamp, so only Basecamp's own copy runs. The copy that "
                          "was in %2 is now in %3.%4")
        .arg(copy.name, from, QDir::toNativeSeparators(copy.to), stopped);
}

} // namespace logos::builtin
