#pragma once

#include <QStringList>

class QQmlEngine;

// The ui_qml sandbox policy, factored out of PluginLoader so it can be unit
// tested against a bare QQmlEngine (it depends only on Qt, never on the SDK,
// liblogos or the rest of basecamp).
namespace QmlSandbox {

// Applies the sandbox to a freshly-created QML engine:
//   * import paths  — module dir(s) first, then the vetted appLibDir, then Qt's
//                     defaults, so the module's own QML/JS resolves;
//   * a deny-all QQmlNetworkAccessManagerFactory (no network);
//   * a RestrictedUrlInterceptor confining file/qmldir resolution to the
//     module's dir + the vetted/Qt roots, and forbidding a qmldir under the
//     module's (untrusted) dir from declaring a native C++ plugin.
//
// Deliberately does NOT add the untrusted installDir to the engine's native
// plugin search path, and keeps Qt's default pluginPathList intact.
//
// `appLibDir` is the vetted application library dir (in production
// <appDir>/../lib); tests pass an explicit value (often empty). Returns the set
// of import roots that were treated as untrusted — useful for assertions.
QStringList configure(QQmlEngine* engine,
                      const QString& installDir,
                      const QString& qmlViewPath,
                      const QString& appLibDir);

} // namespace QmlSandbox
