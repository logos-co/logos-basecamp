#pragma once

#include "InstalledPlacement.h"

#include <QString>
#include <QStringList>

// Launch-time integrity of what the package manager installed: before Basecamp
// loads a package from the user's install dirs, its files are re-hashed against
// the manifest lgpm wrote there and, for a signed package, the signature is
// checked against that manifest. Anything outside those dirs -- bundled with
// the app, or a standalone-mode root -- was never installed and is not checked.
//
// It catches corruption and edits made after install. It is not a defence
// against something that can already write the user's files: that can rewrite
// the manifest too.
namespace logos::integrity {

Problem examine(const QString& packageDir);
inline QString problemWith(const QString& packageDir) { return examine(packageDir).what; }
Problem problemWithAny(const QStringList& paths, const QString& installRoot,
                       QString* failedPackageDir = nullptr);
Preflight preflight(const ICoreRuntime& core, const QString& moduleName,
                    const QString& installRoot);
void preflightInBackground(const QString& modulesInfoJson, const QString& moduleName,
                           const QString& installRoot, QObject* context,
                           std::function<void(const Preflight&)> done);

// The directory of the copy of core module `name` the core runs, of those in
// `modulesDirs` in search order: as the package manager picks, a later copy
// wins unless an earlier one is newer. Empty if there is none.
QString copyThatRuns(const QString& name, const QStringList& modulesDirs);

} // namespace logos::integrity
