#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class ICoreRuntime;
class QObject;

// Where a module sits relative to the user's install dir, which installed
// packages a load would bring up, and what the user is told when one fails.
// No hashing here (see InstalledIntegrity.h): the check is passed in, so this
// part needs nothing but Qt.
namespace logos::integrity {

struct Placement {
    bool inside = false;   // anywhere under the install root
    QString packageDir;    // the package directory; empty if the plugin is loose in the root
};

Placement placementUnder(const QString& pluginPath, const QString& installRoot);

// What a check found wrong with an installed package; nothing when `what` is
// empty. Built from a bare QString, it is a change to the files.
struct Problem {
    enum class Kind {
        Changed,       // the files, or the manifest they are checked against, changed
        Unverifiable,  // what the check needs is missing or malformed
        Unchecked,     // the check itself could not run
    };
    Problem(QString what_ = {}, Kind kind_ = Kind::Changed)
        : what(std::move(what_)), kind(kind_) {}
    bool isEmpty() const { return what.isEmpty(); }
    QString what;
    Kind kind;
};

using PackageCheck = std::function<Problem(const QString& packageDir)>;
Problem problemWithPlugin(const QString& pluginPath, const QString& installRoot,
                          const PackageCheck& check);

// The first problem with the installed packages any of `paths` sits in, each
// package checked once; paths outside the root are not checked.
// `failedPackageDir`, if given, is set to the package that failed.
Problem problemWithAny(const QStringList& paths, const QString& installRoot,
                       const PackageCheck& check, QString* failedPackageDir = nullptr);

struct Preflight {
    QString refusal;          // "module: problem" when a required module failed
    QString withoutOptional;  // the same, when only an optional one did
    QString culprit;          // the module that failed, if any
    QString culpritDir;       // its package directory, when it has one
    Problem problem;
};
Preflight preflight(const QString& modulesInfoJson, const QString& moduleName,
                    const QString& installRoot, const PackageCheck& check);
Preflight preflight(const ICoreRuntime& core, const QString& moduleName,
                    const QString& installRoot, const PackageCheck& check);

// preflight() on the global thread pool, so the calling thread never reads or
// hashes a file; `done` then runs on the main thread, unless `context` is
// gone by then. Takes the module info as a snapshot: nothing off the main
// thread touches the core.
void preflightInBackground(const QString& modulesInfoJson, const QString& moduleName,
                           const QString& installRoot, const PackageCheck& check,
                           QObject* context, std::function<void(const Preflight&)> done);

// What the user is told when `module` was not started because `culprit` --
// the module itself, or one it needs -- failed its check.
QString refusalMessage(const QString& module, const QString& culprit,
                       const QString& culpritDir, const Problem& problem);

// What the user is told when a failing `culprit` kept the installed modules
// dir out of the session at startup.
QString installedModulesSkippedMessage(const QString& culprit, const Problem& problem);

// Found while Basecamp starts, shown once its window is up.
struct StartupNotice {
    QString title;
    QString module;
    QString message;
};
void addStartupNotice(const StartupNotice& notice);
QList<StartupNotice> takeStartupNotices();

} // namespace logos::integrity
