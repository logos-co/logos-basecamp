#include "restricted/RestrictedUrlInterceptor.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include "restricted/SandboxLogging.h"
#include "restricted/SharedLogosModules.h"

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
                                                   const QStringList& untrustedRoots,
                                                   const QString& pluginLabel)
    : m_pluginLabel(pluginLabel)
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

QString RestrictedUrlInterceptor::labelSuffix() const
{
    return m_pluginLabel.isEmpty()
        ? QString()
        : QStringLiteral(" [plugin=%1]").arg(m_pluginLabel);
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

// Detects "the plugin tree is trying to answer for a Logos.<Reserved> import."
// A raw local path is checked against every untrusted root — if it lives under
// one and the sub-path (case-folded) begins with "Logos/<Reserved>/…" or is
// exactly "Logos/<Reserved>", the caller returns an invalid QUrl so Qt keeps
// walking the import path and resolves the module from appLibDir instead.
//
// Uses the raw (pre-canonicalisation) path on purpose: on case-insensitive
// filesystems a leftover qml/icons/ inside the plugin can be Qt's probe target
// while looking for Logos.Icons, and that probe is what needs shadowing before
// Qt reaches deeper into the (junk) directory tree.
bool RestrictedUrlInterceptor::isReservedLogosProbeInUntrustedRoot(
    const QString& localPath, QString* matchedName) const
{
    for (const QString& root : m_untrustedRoots) {
        if (root.isEmpty()) continue;
        if (localPath != root && !localPath.startsWith(root + QLatin1Char('/'))) continue;
        const QString rel = localPath == root
            ? QString()
            : localPath.mid(root.size() + 1);
        if (!rel.startsWith(QStringLiteral("Logos/"), Qt::CaseInsensitive)
            && rel.compare(QStringLiteral("Logos"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString afterLogos = rel.size() > 6 ? rel.mid(6) : QString();
        const QString firstSeg = afterLogos.section(QLatin1Char('/'), 0, 0);
        if (firstSeg.isEmpty()) continue;
        for (const QString& reserved : kSharedLogosModules()) {
            if (firstSeg.compare(reserved, Qt::CaseInsensitive) == 0) {
                if (matchedName) *matchedName = reserved;
                return true;
            }
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

        // Force Logos.<Reserved> to resolve from the vetted appLibDir, never
        // from a plugin tree. This runs before the canonicalise+allowedRoots
        // checks below because it uses the raw probe path — a leftover
        // qml/icons/ inside the plugin can shadow Logos.Icons on
        // case-insensitive filesystems (RCA for basecamp 0.2.2-RC1). By
        // returning an invalid QUrl, Qt treats this candidate as "not here"
        // and continues walking the import path.
        {
            QString reserved;
            if (isReservedLogosProbeInUntrustedRoot(raw, &reserved)) {
                qCWarning(lcBasecampSandbox).noquote()
                    << QStringLiteral("Redirected Logos.%1 probe away from plugin tree \"%2\"%3: "
                                      "shared Logos.* modules resolve from the vetted design-system dir.")
                           .arg(reserved, raw, labelSuffix());
                return QUrl();
            }
        }

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
                << QStringLiteral("Blocked %1 import \"%2\"%3: path exists but could not "
                                  "be canonicalised and cannot be vetted against the "
                                  "sandbox allowed roots.")
                       .arg(QString::fromLatin1(dataTypeName(type)), raw, labelSuffix());
            return QUrl();
        }
        if (!isUnder(local, m_allowedRoots)) {
            qCWarning(lcBasecampSandbox).noquote()
                << QStringLiteral("Blocked %1 import \"%2\"%3: path is outside the basecamp plugin sandbox. "
                                  "Allowed roots: %4")
                       .arg(QString::fromLatin1(dataTypeName(type)),
                            url.toLocalFile(),
                            labelSuffix(),
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
                << QStringLiteral("Blocked qmldir \"%1\"%2: a sandboxed ui_qml module may not declare a "
                                  "native plugin (QML-only modules cannot load C++ plugins into the host).")
                       .arg(url.toLocalFile(), labelSuffix());
            return QUrl();
        }
        return url;
    }

    qCWarning(lcBasecampSandbox).noquote()
        << QStringLiteral("Blocked %1 import \"%2\"%3: scheme \"%4\" is not allowed in the basecamp "
                          "plugin sandbox (only qrc: and local files inside allowed roots are permitted).")
               .arg(QString::fromLatin1(dataTypeName(type)),
                    url.toString(),
                    labelSuffix(),
                    url.scheme());
    return QUrl();  // Block http/https and any other scheme
}
