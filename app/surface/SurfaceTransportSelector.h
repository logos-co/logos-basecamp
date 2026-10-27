#pragma once

// Which SurfaceTransport family a view_surface module is spawned with
// (RFC-6 §3). The platform default, overridable for tests and diagnosis:
//
//   LOGOS_SURFACE_TRANSPORT=wayland   RFC-4 nested compositor (Linux only)
//   LOGOS_SURFACE_TRANSPORT=channel   RFC-7, native slot kind if available
//   LOGOS_SURFACE_TRANSPORT=shm       RFC-7, shm slots only (one copy, every OS)
//   LOGOS_SURFACE_TRANSPORT=none      plain --gui, no transport (RFC-1)
//
// Default: channel on every platform. wayland only where the compositor
// compiled in, came up, and can hand clients GPU buffers
// (LOGOS_SURFACE_GPU_BUFFERS=1 and a working client buffer integration) —
// the one case where it beats channel. The choice is made once per spawn, by
// PluginLoader, and recorded per module so the Logos.Surface item builds the
// matching backend.

#include <QHash>
#include <QString>

class SurfaceTransportSelector {
public:
    struct Selection {
        QString family;        // "wayland" | "channel" | ""
        bool shmOnly = false;  // channel: never negotiate a native kind
    };

    // The family to spawn with on this host, honouring the override.
    static Selection select();
    static bool waylandAvailable();

    // PluginLoader records the family each module was spawned with; the item
    // asks before it builds its backend. Empty: not spawned with a transport.
    static void record(const QString& module, const QString& family);
    static void forget(const QString& module);
    static QString familyFor(const QString& module);

private:
    static QHash<QString, QString>& table();
};
