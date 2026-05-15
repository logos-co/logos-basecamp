#include "restricted/RestrictedUrlInterceptor.h"

#include <QDir>
#include <QLoggingCategory>

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
}

RestrictedUrlInterceptor::RestrictedUrlInterceptor(const QStringList& allowedRoots)
{
    for (const QString& root : allowedRoots) {
        const QString canonical = QDir(root).canonicalPath();
        if (!canonical.isEmpty()) {
            m_allowedRoots.append(canonical);
        }
    }
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
        const QString local = QDir(url.toLocalFile()).canonicalPath();
        if (local.isEmpty()) {
            return url;
        }
        for (const QString& root : m_allowedRoots) {
            if (!root.isEmpty() && (local == root || local.startsWith(root + QLatin1Char('/')))) {
                return url;
            }
        }
        qCWarning(lcBasecampSandbox).noquote()
            << QStringLiteral("Blocked %1 import \"%2\": path is outside the basecamp plugin sandbox. "
                              "Allowed roots: %3")
                   .arg(QString::fromLatin1(dataTypeName(type)),
                        url.toLocalFile(),
                        m_allowedRoots.join(QStringLiteral(", ")));
        return QUrl();  // Block file access outside allowed roots
    }

    qCWarning(lcBasecampSandbox).noquote()
        << QStringLiteral("Blocked %1 import \"%2\": scheme \"%3\" is not allowed in the basecamp "
                          "plugin sandbox (only qrc: and local files inside allowed roots are permitted).")
               .arg(QString::fromLatin1(dataTypeName(type)),
                    url.toString(),
                    url.scheme());
    return QUrl();  // Block http/https and any other scheme
}
