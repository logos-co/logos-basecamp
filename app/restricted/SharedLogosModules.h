#pragma once

#include <QString>
#include <QStringList>

// Canonical enumeration of the shared Logos.* QML modules published by the
// design system. Used in two places that must not drift:
//
//   * QmlSandbox::configure — decides which Logos.<X> subdirs of the vetted
//     appLibDir are added to allowedRoots (so `import Logos.<X>` resolves at all).
//   * RestrictedUrlInterceptor — forces `import Logos.<X>` to resolve from
//     appLibDir even when a plugin tree contains a colliding path, e.g. an
//     asset-only qml/icons/ under an untrusted root. Design system owns the
//     Logos.* namespace; the plugin doesn't get to answer for it.
//
// Extend this list when the design system publishes a new module.
inline const QStringList& kSharedLogosModules()
{
    static const QStringList v = {
        QStringLiteral("Theme"),
        QStringLiteral("Controls"),
        QStringLiteral("Icons"),
    };
    return v;
}
