#include "restricted/QmlSandbox.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QQmlEngine>

#include "restricted/DenyAllNAMFactory.h"
#include "restricted/RestrictedUrlInterceptor.h"

namespace QmlSandbox {

QStringList configure(QQmlEngine* engine,
                      const QString& installDir,
                      const QString& qmlViewPath,
                      const QString& appLibDir)
{
    const QStringList qtDefaultPaths = engine->importPathList();

    // Import paths: the module's own dir(s) first, then the vetted app lib dir,
    // then Qt's defaults. The module needs its install dir on the import path so
    // its own QML/JS resolves; the interceptor below is what keeps that from
    // being abused to load native code.
    QStringList importPaths = qtDefaultPaths;
    importPaths.prepend(installDir);
    const QString qmlEntryDir = QFileInfo(qmlViewPath).absolutePath();
    if (!qmlEntryDir.isEmpty() && qmlEntryDir != installDir)
        importPaths.prepend(qmlEntryDir);
    if (!appLibDir.isEmpty())
        importPaths.prepend(appLibDir);
    engine->setImportPathList(importPaths);

    // SECURITY: do NOT add the (untrusted) module install dir to the native
    // plugin search path. Qt's default pluginPathList is kept intact so vetted
    // modules — QtQuick and friends, whose qmldir legitimately declares an
    // `optional plugin` resolved relative to their own dir — still load. The
    // module's own dir is deliberately absent here; combined with the qmldir
    // check in RestrictedUrlInterceptor below, a sandboxed ui_qml module cannot
    // get a native .so dlopen()ed into the host process. (Previously installDir
    // was prepended here, which — together with the module dir being on the
    // import path — let a qmldir 'plugin' directive load arbitrary native code.)
    engine->setNetworkAccessManagerFactory(new DenyAllNAMFactory());

    // allowedRoots gate ALL local file/qmldir resolution. untrustedRoots is the
    // subset (the module's own dirs) where a qmldir may not declare a native
    // plugin — Qt would otherwise dlopen it without consulting this interceptor.
    QStringList allowedRoots;
    QStringList untrustedRoots;
    allowedRoots << installDir;
    untrustedRoots << installDir;
    if (!qmlEntryDir.isEmpty() && qmlEntryDir != installDir) {
        if (!allowedRoots.contains(qmlEntryDir))
            allowedRoots << qmlEntryDir;
        if (!untrustedRoots.contains(qmlEntryDir))
            untrustedRoots << qmlEntryDir;
    }
    // Allow only an explicit set of shared Logos QML modules.
    if (!appLibDir.isEmpty()) {
        static const QStringList kAllowedLogosModules = {
            QStringLiteral("Theme"),
            QStringLiteral("Controls"),
            QStringLiteral("Icons"),
        };
        for (const QString& mod : kAllowedLogosModules) {
            const QString modDir = QDir(appLibDir + "/Logos/" + mod).canonicalPath();
            if (!modDir.isEmpty() && !allowedRoots.contains(modDir))
                allowedRoots << modDir;
        }
    }
    // TODO(security): currently allows ALL of Qt's default QML module paths.
    // Before opening the platform to third-party plugin publishing, narrow
    // this to an explicit per-module allowlist
    for (const QString& p : qtDefaultPaths) {
        if (p.startsWith(QStringLiteral("qrc:"))) continue;
        const QString canon = QDir(p).canonicalPath();
        if (!canon.isEmpty() && !allowedRoots.contains(canon))
            allowedRoots << canon;
    }
    engine->addUrlInterceptor(new RestrictedUrlInterceptor(allowedRoots, untrustedRoots));
    return untrustedRoots;
}

} // namespace QmlSandbox
