#pragma once

#include <QQmlAbstractUrlInterceptor>
#include <QString>
#include <QStringList>
#include <QUrl>

// Gates every URL the QML engine resolves for a sandboxed ui_qml module.
//
// Three layers:
//   * Logos.* redirect — probes into an untrusted root that would try to
//                        resolve a shared design-system module (Logos.Icons,
//                        Logos.Theme, Logos.Controls, …) are blocked so Qt
//                        continues walking the import path and hits the vetted
//                        appLibDir copy. The design system owns the Logos.*
//                        namespace; a plugin's local folders never answer for
//                        it, regardless of what they contain or how the
//                        filesystem case-folds names.
//   * allowedRoots   — only qrc: URLs and local files under one of these
//                      canonical roots are resolved; everything else (other
//                      schemes, files outside the roots) is blocked.
//   * untrustedRoots — a subset of the allowed roots whose qmldir files may NOT
//                      declare a native `plugin` directive. A qmldir under an
//                      untrusted root that lists a C++ plugin is rejected, which
//                      stops Qt from ever resolving and dlopen()ing that native
//                      library into the host process. This is what confines a
//                      ui_qml module to QML/JS: the module's own install dir is
//                      untrusted, while the app's vetted lib dir and Qt's own
//                      module dirs (which legitimately ship native plugins such
//                      as QtQuick) are trusted and unaffected.
//
// Native plugin loading bypasses url interception entirely (Qt resolves the
// plugin path internally and hands it to QPluginLoader), so blocking the .so
// URL here would be useless — the qmldir that *declares* it is the only choke
// point the interceptor sees, hence the qmldir-level check.
//
// `pluginLabel` is threaded through so every diagnostic can identify which
// plugin engine the probe came from (empty = unlabeled, used by tests).
class RestrictedUrlInterceptor : public QQmlAbstractUrlInterceptor {
public:
    explicit RestrictedUrlInterceptor(const QStringList& allowedRoots,
                                      const QStringList& untrustedRoots = {},
                                      const QString& pluginLabel = QString());
    QUrl intercept(const QUrl& url, DataType type) override;

private:
    bool isUnder(const QString& canonicalPath, const QStringList& roots) const;
    bool qmldirDeclaresNativePlugin(const QString& qmldirPath) const;
    bool isReservedLogosProbeInUntrustedRoot(const QString& localPath,
                                             QString* matchedName) const;
    QString labelSuffix() const;

    QStringList m_allowedRoots;
    QStringList m_untrustedRoots;
    QString m_pluginLabel;
};
