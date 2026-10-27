#include "SurfaceTransportSelector.h"

#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
#include "SurfaceCompositor.h"
#endif

#include <QLoggingCategory>
#include <QtGlobal>

Q_LOGGING_CATEGORY(lcSurfaceSelect, "basecamp.surface.select")

// Queries (the item) never start a compositor; only a spawn decision does.
bool SurfaceTransportSelector::waylandAvailable()
{
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
    SurfaceCompositor* compositor = SurfaceCompositor::existing();
    return compositor && compositor->isAvailable();
#else
    return false;
#endif
}

namespace {
bool startWayland()
{
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
    SurfaceCompositor* compositor = SurfaceCompositor::instance();
    return compositor && compositor->isAvailable();
#else
    return false;
#endif
}
} // namespace

SurfaceTransportSelector::Selection SurfaceTransportSelector::select()
{
    Selection s;
    const QString override = qEnvironmentVariable("LOGOS_SURFACE_TRANSPORT").trimmed().toLower();
    if (override == QLatin1String("none")) {
        return s;
    }
    if (override == QLatin1String("wayland")) {
        if (startWayland()) { s.family = QStringLiteral("wayland"); return s; }
        qCWarning(lcSurfaceSelect) << "LOGOS_SURFACE_TRANSPORT=wayland but no compositor; using channel";
        s.family = QStringLiteral("channel");
        return s;
    }
    if (override == QLatin1String("channel") || override == QLatin1String("shm")) {
        s.family = QStringLiteral("channel");
        s.shmOnly = override == QLatin1String("shm");
        return s;
    }
    if (!override.isEmpty())
        qCWarning(lcSurfaceSelect) << "unknown LOGOS_SURFACE_TRANSPORT" << override << "- using the default";

    // Default (RFC-6 §3): `channel` on every platform — one code path from
    // Linux to Windows and macOS, the view renders on the GPU it finds in its
    // own session. `wayland` only when it brings what channel cannot: GPU
    // buffers shared with the host, zero-copy. That needs the host to allow
    // them (LOGOS_SURFACE_GPU_BUFFERS=1) and the compositor's client buffer
    // integration to come up; a wl_shm-only nested compositor would leave
    // the client with software GL (Mesa offers no GPU context to a Wayland
    // client whose compositor has no wl_drm / linux-dmabuf), which is slower
    // than channel-shm by two orders of magnitude on a map.
#if defined(LOGOS_HAVE_WAYLAND_COMPOSITOR)
    if (SurfaceCompositor::gpuBuffersRequested() && startWayland()) {
        SurfaceCompositor* compositor = SurfaceCompositor::existing();
        if (compositor && compositor->gpuBuffersAvailable()) {
            s.family = QStringLiteral("wayland");
            return s;
        }
        qCInfo(lcSurfaceSelect) << "GPU buffers requested but the compositor has no client buffer"
                                   " integration; using channel";
    }
#endif
    s.family = QStringLiteral("channel");
    return s;
}

QHash<QString, QString>& SurfaceTransportSelector::table()
{
    static QHash<QString, QString> t;
    return t;
}

void SurfaceTransportSelector::record(const QString& module, const QString& family)
{
    if (family.isEmpty()) table().remove(module);
    else table().insert(module, family);
}

void SurfaceTransportSelector::forget(const QString& module)
{
    table().remove(module);
}

QString SurfaceTransportSelector::familyFor(const QString& module)
{
    return table().value(module);
}
