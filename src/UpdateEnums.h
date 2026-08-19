#pragma once

#include <QObject>
#include <QtQml/qqml.h>

// Host-app update state, exposed to QML so DashboardView can switch on it by
// name instead of magic ints. Same Q_ENUM + QML_UNCREATABLE shape as
// InstallStage/InstallStatus in InstallEnums.h.

// Result of the version check itself.
class UpdateCheckState : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use UpdateCheckState.Checking etc.; not instantiable.")
public:
    enum Value {
        Idle = 0,
        // Terminal, set without any network call: this build has no comparable
        // version (dev / pre-release / non-nix) or the check is disabled.
        Skipped,
        Checking,
        UpToDate,
        UpdateAvailable,
        // Network, TLS, rate-limit or parse failure. Deliberately distinct from
        // UpToDate — a failed check must never read as "you're on the latest".
        Failed,
    };
    Q_ENUM(Value)
};

// Progress of the optional asset download.
class UpdateDownloadStage : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use UpdateDownloadStage.Downloading etc.; not instantiable.")
public:
    enum Value {
        Idle = 0,
        // No release asset matches this platform (Intel Mac ships no x86_64 dmg,
        // Windows ships nothing). Renders the link-out, not an error.
        Unsupported,
        Starting,
        Downloading,
        // Dormant until the .sha256 follow-up. Kept so that lands as a C++-only
        // change with no QML edit.
        Verifying,
        Done,
        Failed,
    };
    Q_ENUM(Value)
};
