#pragma once

// The host side of RFC-4: a nested Wayland compositor that lives inside the
// Basecamp process so a view module's backend (running in its own ui-host
// process under RFC-1 `--gui`) can hand its rendered frames to the host with at
// most one copy, and get input back, without any module code entering this
// process. The compositor is a server; it works whether Basecamp itself runs
// under Wayland, X11 or the offscreen QPA.
//
// Admission is by RFC-2 token: PluginLoader calls expect() with the token it
// minted for a spawn and the pid of the ui-host it spawned. A client whose pid
// is not expected is closed as soon as it asks for its first surface; a
// toplevel whose xdg app_id is not "logos-view-<expected token>" from the
// expected pid is closed too. Only a bound toplevel is ever shown, and only by
// the Logos.Surface item (LogosSurfaceItem) of the module that owns the token.
//
// Compiled only where Qt6::WaylandCompositor exists (Linux); the item degrades
// to `transport: none` elsewhere.

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QString>

class QQuickWindow;
class QWaylandClient;
class QWaylandCompositor;
class QWaylandOutput;
class QWaylandQuickOutput;
class QWaylandSurface;
class QWaylandXdgShell;
class QWaylandXdgSurface;
class QWaylandXdgToplevel;

class SurfaceCompositor : public QObject {
    Q_OBJECT

public:
    // Lazily creates the process-wide compositor (parented to the
    // application) the first time a view_surface module is spawned.
    static SurfaceCompositor* instance();
    // The compositor if one has been created, else nullptr. Items use this:
    // a Logos.Surface in a host that never spawned a gui module must not be
    // the thing that starts a compositor.
    static SurfaceCompositor* existing();

    ~SurfaceCompositor() override;

    // False when the socket could not be created (no usable XDG_RUNTIME_DIR,
    // or Qt's compositor failed to come up). Spawns then run with --gui but
    // without --surface-socket, and the item reports `noCompositor`.
    bool isAvailable() const;
    QString socketName() const;

    // Whether a client of this compositor can get GPU buffers: the host
    // allowed them (LOGOS_SURFACE_GPU_BUFFERS=1, H2) AND Qt's client buffer
    // integration came up (the host's EGL exports EGL_WL_bind_wayland_display,
    // so wl_drm / zwp_linux_dmabuf_v1 exist to bind). Without this a client
    // sees wl_shm only, and Mesa then gives it no GPU context at all — the
    // module renders in software (llvmpipe). That makes this the condition
    // for choosing the Wayland transport by default (RFC-6 §3): the one
    // thing it does better than `channel` is zero-copy GPU buffers, and
    // without them it is strictly worse.
    bool gpuBuffersAvailable() const;
    static bool gpuBuffersRequested();

    // Admission table. expect() before the client can connect — i.e. right
    // after ViewModuleHost::spawn() returns, with the token it minted — and
    // forget() before ViewModuleHost::stop(), so the disconnect that follows
    // is not reported as a lost surface.
    void expect(const QString& module, const QString& token, qint64 pid);
    void forget(const QString& token);
    bool isExpected(const QString& token) const;
    // The token expected for a module name, or empty. This is how a
    // Logos.Surface item finds its own backend: the sandbox tells the item
    // which module it belongs to, the item asks here.
    QString tokenFor(const QString& module) const;

    // The admitted toplevel for a token, once it has been created.
    QWaylandXdgSurface* boundSurface(const QString& token) const;
    QWaylandXdgToplevel* boundToplevel(const QString& token) const;

    // One QWaylandQuickOutput per host QQuickWindow that shows a surface
    // (Spike A shape A1: the output's window is the sandboxed QQuickWidget's
    // own offscreen QQuickWindow). Idempotent. Frame callbacks for the
    // surfaces in that window are sent from its afterRendering, so a window
    // that does not render (a hidden tab) pauses its clients for free.
    void ensureOutput(QQuickWindow* window);
    QWaylandOutput* outputFor(QQuickWindow* window) const;

    QWaylandCompositor* compositor() const { return m_compositor; }

    // RFC-4 rev 2 H2: which globals a client may see and bind. The protocol
    // one Qt toplevel needs is always offered; the GPU buffer path
    // (zwp_linux_dmabuf_v1, wl_drm, qt_hardware_integration) only when the
    // host allows it. A property of this instance, not of the environment
    // the child could influence; initialised from LOGOS_SURFACE_GPU_BUFFERS=1.
    void setGpuBuffersAllowed(bool allowed) { m_gpuBuffers = allowed; }
    bool gpuBuffersAllowed() const { return m_gpuBuffers; }
    static bool isGlobalAllowed(const char* interfaceName, bool gpuBuffers);

    // RFC-4 rev 2 H3: the largest buffer a client may commit; a larger one
    // closes the client before Qt uploads it.
    static constexpr int kMaxBufferSide = 8192;
    static constexpr qint64 kMaxBufferBytes = qint64(256) * 1024 * 1024;
    // Per-connection receive buffer cap (wl_client_set_max_buffer_size).
    static constexpr size_t kMaxClientBufferBytes = size_t(1) << 20;

    // Test seam: the number of clients/toplevels refused so far.
    int refusedCount() const { return m_refused; }

signals:
    void surfaceBound(const QString& token);
    // Emitted only while the token is still expected: the client went away
    // on its own (crash, exit). After forget() nothing is emitted.
    void surfaceLost(const QString& token);
    // A connection or toplevel that failed admission. `appId` is empty for
    // the pid gate.
    void surfaceRefused(qint64 pid, const QString& appId);

private:
    explicit SurfaceCompositor(QObject* parent);

    struct Expected {
        QString module;
        QString token;
        qint64 pid = 0;
    };
    struct Bound {
        QPointer<QWaylandXdgToplevel> toplevel;
        QPointer<QWaylandXdgSurface> xdgSurface;
    };

    void onSurfaceRequested(QWaylandClient* client, uint id, int version);
    void onToplevelCreated(QWaylandXdgToplevel* toplevel, QWaylandXdgSurface* xdgSurface);
    void tryAdmit(QWaylandXdgToplevel* toplevel, QWaylandXdgSurface* xdgSurface, bool final);
    void refuse(QWaylandClient* client, const QString& appId);
    void onBoundGone(const QString& token);
    void checkBufferBounds(const QString& token, QWaylandSurface* surface);
    static bool bufferWithinBounds(const QSize& size);
    int scaleFor(QQuickWindow* window) const;

    QWaylandCompositor* m_compositor = nullptr;
    QWaylandXdgShell* m_xdgShell = nullptr;
    QWaylandOutput* m_headlessOutput = nullptr;
    bool m_available = false;
    bool m_gpuBuffers = false;
    int m_refused = 0;
    // Clients whose receive buffer has been capped (H3).
    QSet<QWaylandClient*> m_cappedClients;

    QHash<QString, Expected> m_expected;   // token -> expectation
    QHash<QString, QString> m_tokenByModule;
    QHash<QString, Bound> m_bound;         // token -> admitted toplevel
    // Toplevels seen but not yet admitted (app_id arrives after creation).
    QHash<QWaylandXdgToplevel*, QPointer<QWaylandXdgSurface>> m_pending;
    // Clients already refused and awaiting their deferred close.
    QSet<QWaylandClient*> m_refusedClients;
};
