#include "InstalledPlacement.h"

#include "interfaces/ICoreRuntime.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QThreadPool>

#include <utility>

namespace logos::integrity {

namespace {

// The filesystems these run on by default ignore case, so the paths do too.
// On a case-sensitive volume that only ever checks MORE, never less.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

// Absolute, `..` collapsed textually: the path as its caller spelled it.
QString written(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

// Symlinks resolved by the filesystem; empty if the path does not exist.
QString resolved(const QString& path)
{
    return QFileInfo(path).canonicalFilePath();
}

QStringList forms(const QString& path)
{
    QStringList out{written(path)};
    const QString real = resolved(path);
    if (!real.isEmpty() && real != out.first())
        out << real;
    return out;
}

const Problem kLoose{QStringLiteral("it sits loose in the install directory, not in an installed package"),
                     Problem::Kind::Unverifiable};

} // namespace

Placement placementUnder(const QString& pluginPath, const QString& installRoot)
{
    if (pluginPath.isEmpty() || installRoot.isEmpty())
        return {};
    for (const QString& root : forms(installRoot)) {
        const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        for (const QString& plugin : forms(pluginPath)) {
            if (!plugin.startsWith(prefix, kPathCase))
                continue;
            const QString rel = plugin.mid(prefix.size());
            const int slash = rel.indexOf(QLatin1Char('/'));
            if (slash < 0)
                return {true, {}};
            return {true, prefix + rel.left(slash)};
        }
    }
    return {};
}

Problem problemWithPlugin(const QString& pluginPath, const QString& installRoot,
                          const PackageCheck& check)
{
    const Placement where = placementUnder(pluginPath, installRoot);
    if (!where.inside)
        return {};
    if (where.packageDir.isEmpty())
        return kLoose;
    return check(where.packageDir);
}

Problem problemWithAny(const QStringList& paths, const QString& installRoot,
                       const PackageCheck& check, QString* failedPackageDir)
{
    QStringList checked;
    for (const QString& path : paths) {
        const Placement where = placementUnder(path, installRoot);
        if (!where.inside)
            continue;
        if (where.packageDir.isEmpty())
            return kLoose;
        if (checked.contains(where.packageDir, kPathCase))
            continue;
        checked << where.packageDir;
        const Problem problem = check(where.packageDir);
        if (!problem.isEmpty()) {
            if (failedPackageDir)
                *failedPackageDir = where.packageDir;
            return problem;
        }
    }
    return {};
}

Preflight preflight(const QString& modulesInfoJson, const QString& moduleName,
                    const QString& installRoot, const PackageCheck& check)
{
    if (installRoot.isEmpty())
        return {};

    struct Info {
        QString path;
        bool loaded = false;
        QStringList required;
        QStringList optional;
    };
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(modulesInfoJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        Preflight unlisted;
        unlisted.culprit = moduleName;
        unlisted.problem = Problem(
            QStringLiteral("the installed modules could not be listed, so none could be checked"),
            Problem::Kind::Unchecked);
        unlisted.refusal = moduleName + QStringLiteral(": ") + unlisted.problem.what;
        return unlisted;
    }

    QHash<QString, Info> modules;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        Info info;
        info.path = o.value(QStringLiteral("path")).toString();
        info.loaded = o.value(QStringLiteral("loaded")).toBool();
        for (const QJsonValue& d : o.value(QStringLiteral("dependencies")).toArray())
            info.required << d.toString();
        for (const QJsonValue& d : o.value(QStringLiteral("optional_dependencies")).toArray())
            info.optional << d.toString();
        modules.insert(o.value(QStringLiteral("name")).toString(), info);
    }

    Preflight result;
    // Breadth-first from `roots` over required edges, collecting the optional
    // edges found on the way into `optionalRoots`. Without `optionalRoots`
    // optional edges are followed too: everything that pass reaches is
    // optional, and the core loads optional dependencies to any depth.
    // Records the first failure in `result`; true if there was none.
    QSet<QString> seen;
    auto walk = [&](QStringList queue, QStringList* optionalRoots) -> bool {
        while (!queue.isEmpty()) {
            const QString name = queue.takeFirst();
            if (seen.contains(name))
                continue;
            seen.insert(name);
            const auto it = modules.constFind(name);
            if (it == modules.constEnd())
                continue;  // unknown: the core refuses it on its own
            if (!it->loaded) {
                const Placement where = placementUnder(it->path, installRoot);
                const Problem problem = !where.inside ? Problem()
                                      : where.packageDir.isEmpty() ? kLoose
                                      : check(where.packageDir);
                if (!problem.isEmpty()) {
                    result.culprit = name;
                    result.culpritDir = where.packageDir;
                    result.problem = problem;
                    return false;
                }
            }
            queue << it->required;
            if (optionalRoots)
                *optionalRoots << it->optional;
            else
                queue << it->optional;
        }
        return true;
    };

    // What the module needs: any failure refuses the load. Then what it can
    // use: a failure there only drops the optional collaborators, as the core
    // does with one that fails to load -- it never refuses the module.
    QStringList optionalRoots;
    if (!walk({moduleName}, &optionalRoots))
        result.refusal = result.culprit + QStringLiteral(": ") + result.problem.what;
    else if (!walk(optionalRoots, nullptr))
        result.withoutOptional = result.culprit + QStringLiteral(": ") + result.problem.what;
    return result;
}

Preflight preflight(const ICoreRuntime& core, const QString& moduleName,
                    const QString& installRoot, const PackageCheck& check)
{
    if (installRoot.isEmpty())
        return {};
    return preflight(core.modulesInfoJson(), moduleName, installRoot, check);
}

void preflightInBackground(const QString& modulesInfoJson, const QString& moduleName,
                           const QString& installRoot, const PackageCheck& check,
                           QObject* context, std::function<void(const Preflight&)> done)
{
    // `guard` is only read on the main thread, which owns `context`.
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start(
        [modulesInfoJson, moduleName, installRoot, check, guard, done]() {
            const Preflight result = preflight(modulesInfoJson, moduleName, installRoot, check);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, done, result]() {
                if (guard)
                    done(result);
            }, Qt::QueuedConnection);
        });
}

namespace {

// Package Manager cannot repair the modules it runs on, nor itself.
bool repairsItself(const QString& module)
{
    return module == QLatin1String("package_manager")
        || module == QLatin1String("package_downloader")
        || module == QLatin1String("package_manager_ui");
}

} // namespace

QString refusalMessage(const QString& module, const QString& culprit,
                       const QString& culpritDir, const Problem& problem)
{
    const bool itself = culprit.isEmpty() || culprit == module;
    QString headline;
    switch (problem.kind) {
    case Problem::Kind::Changed:
        headline = itself
            ? QStringLiteral("It was not started because its files changed after it was installed.")
            : QStringLiteral("It was not started because a module it needs, %1, changed after it was installed.").arg(culprit);
        break;
    case Problem::Kind::Unverifiable:
        headline = itself
            ? QStringLiteral("It was not started because its installed files cannot be verified.")
            : QStringLiteral("It was not started because a module it needs, %1, cannot be verified.").arg(culprit);
        break;
    case Problem::Kind::Unchecked:
        headline = itself
            ? QStringLiteral("It was not started because its files could not be checked.")
            : QStringLiteral("It was not started because a module it needs, %1, could not be checked.").arg(culprit);
        break;
    }

    const QString target = itself ? QStringLiteral("it") : culprit;
    const QString broken = itself ? module : culprit;
    QString advice;
    if (repairsItself(broken) && !culpritDir.isEmpty()) {
        advice = QStringLiteral("Package Manager cannot repair %1: remove %2 and restart "
                                "Basecamp to use the copy that came with it.")
                     .arg(broken, QDir::toNativeSeparators(culpritDir));
    } else if (problem.kind == Problem::Kind::Unchecked) {
        advice = QStringLiteral("Try again. If it keeps failing, reinstall %1 in Package Manager.").arg(target);
    } else {
        advice = QStringLiteral("Reinstall %1 in Package Manager.").arg(target);
    }
    return headline + QLatin1Char(' ') + advice + QStringLiteral("\n\n")
         + broken + QStringLiteral(": ") + problem.what;
}

QString installedModulesSkippedMessage(const QString& culprit, const Problem& problem)
{
    const QString why = problem.kind == Problem::Kind::Changed
        ? QStringLiteral("its files changed after it was installed")
        : problem.kind == Problem::Kind::Unverifiable
            ? QStringLiteral("its installed files cannot be verified")
            : QStringLiteral("its files could not be checked");
    return QStringLiteral("Basecamp started without the modules you installed, because "
                          "%1 runs before anything else and %2. Reinstall %1 in Package "
                          "Manager, then restart Basecamp.\n\n%1: %3")
        .arg(culprit, why, problem.what);
}

namespace {

QMutex& noticesMutex()
{
    static QMutex mutex;
    return mutex;
}

QList<StartupNotice>& notices()
{
    static QList<StartupNotice> list;
    return list;
}

} // namespace

void addStartupNotice(const StartupNotice& notice)
{
    QMutexLocker lock(&noticesMutex());
    notices() << notice;
}

QList<StartupNotice> takeStartupNotices()
{
    QMutexLocker lock(&noticesMutex());
    return std::exchange(notices(), {});
}

} // namespace logos::integrity
