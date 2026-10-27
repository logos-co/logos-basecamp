#pragma once

// RFC-5 rev 2 Part C: the one place that decides whether a module gets a
// SurfaceTransport at all, and which. Today the decision is the package's
// declaration (`view_surface: true` in its metadata.json) and the platform
// default from SurfaceTransportSelector. The capability gate — asking
// capability_module whether THIS module may hold `view_surface` for THIS
// user, with the host's consent flow — is specified in RFC-5 §C and plugs in
// here when capability_module grows a host-queryable capability registry;
// nothing else in the loader changes for it.

#include <QString>

struct PluginLoadRequest;

struct SurfaceGrant {
    bool allowed = false;      // spawn with a transport at all
    QString transport;         // RFC-6 §3 family: "wayland" | "channel" | ""
    bool shmOnly = false;      // channel: shm slots only (LOGOS_SURFACE_TRANSPORT=shm)
    QString denialReason;      // for the log and Logos.Surface error("denied")
};

class SurfacePolicy {
public:
    static SurfaceGrant decide(const PluginLoadRequest& request);

    // The capability a package declares for this privilege (RFC-5 §C step 1)
    // and the name under which a module reports what it got (RFC-8).
    static constexpr const char* kCapability = "view_surface";
};
