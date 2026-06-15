#include "restricted/RestrictedUrlInterceptor.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTextStream>

Q_LOGGING_CATEGORY(lcBasecampSandbox, "logos.basecamp.sandbox")

namespace {
const char* dataTypeName(QQmlAbstractUrlInterceptor::DataType type)
{
    switch (type) {
        case QQmlAbstractUrlInterceptor::QmlFile:        return "QML file";
        case QQmlAbstractUrlInterceptor::JavaScriptFile: return "JavaScript file";
        case QQmlAbstractUrlInterceptor::QmldirFile:     return "qmldir file";
        case QQmlAbstractUrlInterceptor::UrlString:      return "URL";
    }
    return "resource";
}

// A qmldir 'plugin' / 'optional plugin' directive declares a native C++ plugin
// that Qt will dlopen() into the host process. Matches the leading directive
// keyword only (per the qmldir grammar each directive is at the start of a line).
const QRegularExpression& nativePluginDirective()
{
    static const QRegularExpression re(QStringLiteral("^\\s*(optional\\s+)?plugin(\\s|$)"));
    return re;
}
}

RestrictedUrlInterceptor::RestrictedUrlInterceptor(const QStringList& allowedRoots,
                                                   const QStringList& untrustedRoots)
{
    for (const QString& root : allowedRoots) {
        const QString canonical = QDir(root).canonicalPath();
        if (!canonical.isEmpty()) {
            m_allowedRoots.append(canonical);
        }
    }
    for (const QString& root : untrustedRoots) {
        const QString canonical = QDir(root).canonicalPath();
        if (!canonical.isEmpty()) {
            m_untrustedRoots.append(canonical);
        }
    }
}

bool RestrictedUrlInterceptor::isUnder(const QString& canonicalPath,
                                       const QStringList& roots) const
{
    for (const QString& root : roots) {
        if (!root.isEmpty()
            && (canonicalPath == root
                || canonicalPath.startsWith(root + QLatin1Char('/')))) {
            return true;
        }
    }
    return false;
}

bool RestrictedUrlInterceptor::qmldirDeclaresNativePlugin(const QString& qmldirPath) const
{
    QFile file(qmldirPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Fail closed: if we cannot read the qmldir to vet it, treat it as if it
        // declared a plugin so a sandboxed module cannot smuggle one past us.
        return true;
    }
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        if (nativePluginDirective().match(stream.readLine()).hasMatch()) {
            return true;
        }
    }
    return false;
}

QUrl RestrictedUrlInterceptor::intercept(const QUrl& url, DataType type)
{
    if (!url.isValid()) {
        return QUrl();
    }

    if (url.scheme() == QLatin1String("qrc")) {
        return url;
    }

    if (url.isLocalFile()) {
        const QString raw   = url.toLocalFile();
        const QString local = QDir(raw).canonicalPath();
        if (local.isEmpty()) {
            // Empty canonical path = the path doesn't exist. Qt's module
            // resolution probes many non-existent qmldir candidates before
            // hitting the real one, so let misses through (they can't load
            // anything; a real hit re-enters here and is vetted then). Only an
            // existing-but-uncanonicalisable path (symlink loop) fails closed.
            if (!QFile::exists(raw)) {
                return url;
            }
            qCWarning(lcBasecampSandbox).noquote()
                << QStringLiteral("Blocked %1 import \"%2\": path exists but could not "
                                  "be canonicalised and cannot be vetted against the "
                                  "sandbox allowed roots.")
                       .arg(QString::fromLatin1(dataTypeName(type)), raw);
            return QUrl();
        }
        if (!isUnder(local, m_allowedRoots)) {
            qCWarning(lcBasecampSandbox).noquote()
                << QStringLiteral("Blocked %1 import \"%2\": path is outside the basecamp plugin sandbox. "
                                  "Allowed roots: %3")
                       .arg(QString::fromLatin1(dataTypeName(type)),
                            url.toLocalFile(),
                            m_allowedRoots.join(QStringLiteral(", ")));
            return QUrl();  // Block file access outside allowed roots
        }

        // A ui_qml module is QML/JS only. A qmldir living in the module's own
        // (untrusted) install dir must not declare a native C++ plugin: Qt
        // resolves that plugin internally and dlopen()s it into the host
        // process WITHOUT routing the .so through this interceptor, so the
        // qmldir is the only place we can stop it. Rejecting the qmldir here
        // makes the module "not installed" instead of granting it native code
        // execution. Trusted roots (the app's vetted lib dir, Qt's own module
        // dirs that legitimately ship plugins like QtQuick) are exempt.
        if (type == QmldirFile && isUnder(local, m_untrustedRoots)
            && qmldirDeclaresNativePlugin(local)) {
            qCWarning(lcBasecampSandbox).noquote()
                << QStringLiteral("Blocked qmldir \"%1\": a sandboxed ui_qml module may not declare a "
                                  "native plugin (QML-only modules cannot load C++ plugins into the host).")
                       .arg(url.toLocalFile());
            return QUrl();
        }
        return url;
    }

    qCWarning(lcBasecampSandbox).noquote()
        << QStringLiteral("Blocked %1 import \"%2\": scheme \"%3\" is not allowed in the basecamp "
                          "plugin sandbox (only qrc: and local files inside allowed roots are permitted).")
               .arg(QString::fromLatin1(dataTypeName(type)),
                    url.toString(),
                    url.scheme());
    return QUrl();  // Block http/https and any other scheme
}
