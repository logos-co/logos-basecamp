#include "restricted/RestrictedUrlInterceptor.h"

#include <QDir>

RestrictedUrlInterceptor::RestrictedUrlInterceptor(const QStringList& allowedRoots)
{
    for (const QString& root : allowedRoots) {
        const QString canonical = QDir(root).canonicalPath();
        if (!canonical.isEmpty()) {
            m_allowedRoots.append(canonical);
        }
    }
}

QUrl RestrictedUrlInterceptor::intercept(const QUrl& url, DataType)
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
        return QUrl();  // Block file access outside allowed roots
    }

    return QUrl();  // Block http/https and any other scheme
}
