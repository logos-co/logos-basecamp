#include "SurfacePolicy.h"

#include "PluginLoader.h"
#include "SurfaceTransportSelector.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSurfacePolicy, "basecamp.surface.policy")

SurfaceGrant SurfacePolicy::decide(const PluginLoadRequest& request)
{
    SurfaceGrant grant;
    // Step 0 (this revision): the package declares it renders its own
    // surface. Without the declaration there is nothing to decide.
    if (!request.viewSurface) {
        grant.denialReason = QStringLiteral("not a view_surface module");
        return grant;
    }

    // Steps 2-4 of RFC-5 §C go here: ask the capability broker for
    // `view_surface` on behalf of the module's trusted package name, show the
    // consent the first time, log the decision. Until capability_module
    // exposes that query the declaration is the grant.

    const SurfaceTransportSelector::Selection sel = SurfaceTransportSelector::select();
    if (sel.family.isEmpty()) {
        // LOGOS_SURFACE_TRANSPORT=none: the module runs --gui with its own
        // window (RFC-1) and Logos.Surface reports no transport.
        grant.denialReason = QStringLiteral("transport disabled (LOGOS_SURFACE_TRANSPORT=none)");
        return grant;
    }
    grant.allowed = true;
    grant.transport = sel.family;
    grant.shmOnly = sel.shmOnly;
    qCInfo(lcSurfacePolicy) << request.name << "granted" << grant.transport
                            << (grant.shmOnly ? "(shm only)" : "");
    return grant;
}
