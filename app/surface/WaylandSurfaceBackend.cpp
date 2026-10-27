#include "WaylandSurfaceBackend.h"

#include "SurfaceCompositor.h"

#include <QLoggingCategory>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWaylandBufferRef>
#include <QWaylandOutput>
#include <QWaylandQuickShellSurfaceItem>
#include <QWaylandSeat>
#include <QWaylandSurface>
#include <QWaylandView>
#include <QWaylandXdgShell>

Q_LOGGING_CATEGORY(lcSurfaceWayland, "basecamp.surface.wayland")

WaylandSurfaceBackend::WaylandSurfaceBackend(QQuickItem* item)
    : SurfaceBackend(item)
{
}

WaylandSurfaceBackend::~WaylandSurfaceBackend()
{
    if (m_shellItem) {
        m_shellItem->setOutput(nullptr);
        delete m_shellItem;
        m_shellItem = nullptr;
    }
}

bool WaylandSurfaceBackend::bind(const QString& module, QString* errorCode)
{
    if (m_shellItem) return true;
    SurfaceCompositor* compositor = SurfaceCompositor::existing();
    if (!compositor || !compositor->isAvailable()) {
        *errorCode = QStringLiteral("noCompositor");
        return false;
    }
    const QString token = compositor->tokenFor(module);
    if (token.isEmpty()) {
        *errorCode = QStringLiteral("notGuiModule");
        return false;
    }
    m_compositor = compositor;
    m_token = token;
    compositor->ensureOutput(m_item->window());
    connect(compositor, &SurfaceCompositor::surfaceLost, this, [this](const QString& t) {
        if (t == m_token) detach(QStringLiteral("clientExited"));
    });
    connect(compositor, &SurfaceCompositor::surfaceBound, this, [this](const QString& t) {
        if (t == m_token && !m_shellItem) attach();
    });
    if (compositor->boundSurface(token)) attach();
    return true;
}

void WaylandSurfaceBackend::unbind()
{
    if (m_compositor) m_compositor->disconnect(this);
    if (m_shellItem) {
        if (m_enterTimer) m_enterTimer->stop();
        m_shellItem->setOutput(nullptr);
        m_shellItem->setVisible(false);
        m_shellItem->deleteLater();
        m_shellItem = nullptr;
    }
    m_toplevel.clear();
    m_xdgSurface.clear();
    setReady(false);
    setTransport(QStringLiteral("none"));
}

void WaylandSurfaceBackend::attach()
{
    if (!m_compositor || m_shellItem) return;
    QWaylandXdgSurface* xdg = m_compositor->boundSurface(m_token);
    QWaylandXdgToplevel* toplevel = m_compositor->boundToplevel(m_token);
    if (!xdg || !toplevel || !xdg->surface()) return;

    m_xdgSurface = xdg;
    m_toplevel = toplevel;

    // The output must exist before the shell item meets its surface; Qt
    // asserts outputFor(window) when it connects the two.
    m_compositor->ensureOutput(m_item->window());

    m_shellItem = new QWaylandQuickShellSurfaceItem(m_item);
    m_shellItem->setSize(m_item->size());
    m_shellItem->setInputEventsEnabled(true);
    m_shellItem->setTouchEventsEnabled(true);
    m_shellItem->setFocusOnClick(true);
    m_shellItem->setShellSurface(xdg);

    QWaylandSurface* surface = xdg->surface();
    auto onContent = [this, surface] {
        if (!m_shellItem || !surface->hasContent()) return;
        const QWaylandBufferRef buffer = m_shellItem->view()->currentBuffer();
        if (buffer.hasBuffer())
            setTransport(buffer.isSharedMemory() ? QStringLiteral("wayland-shm")
                                                 : QStringLiteral("wayland-dmabuf"));
        setReady(true);
    };
    connect(surface, &QWaylandSurface::hasContentChanged, this, onContent);
    connect(surface, &QWaylandSurface::redraw, this, onContent);

    sendConfigure();
    updateOutputGate();
    onContent();

    // RFC-4 rev 2 H4: keyboard focus follows the attached surface. Without
    // this a client that never gets focus-on-click (touch, programmatic
    // show) has no keyboard, and the previous module's surface keeps it.
    if (QWaylandSeat* seat = m_compositor->compositor()->defaultSeat())
        seat->setKeyboardFocus(surface);

    if (!m_enterTimer) {
        m_enterTimer = new QTimer(this);
        m_enterTimer->setInterval(100);
        connect(m_enterTimer, &QTimer::timeout, this, &WaylandSurfaceBackend::ensureEntered);
    }
    m_enterAttempts = 0;
    m_enterTimer->start();
    qCInfo(lcSurfaceWayland) << "attached" << m_token.left(8);
}

void WaylandSurfaceBackend::ensureEntered()
{
    // Qt sends wl_output.enter from the first frame-callback round after the
    // surface has content, and marks it done even when the client has not yet
    // bound that output (the per-window output is created after the client
    // connected, so this is the normal case here). A client that misses the
    // enter keeps rendering at the headless output's scale. Until its buffer
    // scale matches the window output, re-attach the view to the output —
    // which resets Qt's entered flag — and ask for a host frame to resend it.
    if (!m_shellItem || !m_xdgSurface || !m_compositor || !m_item->isVisible() || !m_item->window()) {
        if (m_enterTimer) m_enterTimer->stop();
        return;
    }
    QWaylandOutput* output = m_compositor->outputFor(m_item->window());
    const bool matched = output && m_xdgSurface->surface()->bufferScale() == output->scaleFactor();
    if (matched || ++m_enterAttempts > 30) {
        m_enterTimer->stop();
        if (!matched)
            qCWarning(lcSurfaceWayland) << m_token.left(8) << "client never adopted the output scale";
        return;
    }
    if (output) {
        m_shellItem->setOutput(nullptr);
        m_shellItem->setOutput(output);
        m_shellItem->update();
    }
}

void WaylandSurfaceBackend::detach(const QString& errorCode)
{
    if (!m_shellItem) return;
    if (m_enterTimer) m_enterTimer->stop();
    // RFC-4 rev 2 H4: a surface that is going away gives the keyboard back.
    if (m_compositor && m_compositor->compositor()) {
        if (QWaylandSeat* seat = m_compositor->compositor()->defaultSeat()) {
            if (m_xdgSurface && seat->keyboardFocus() == m_xdgSurface->surface())
                seat->setKeyboardFocus(nullptr);
        }
    }
    // Deleting the item drops its view; setShellSurface(nullptr) on an
    // already-destroyed shell surface only draws a Qt warning.
    m_shellItem->setOutput(nullptr);
    m_shellItem->setVisible(false);
    m_shellItem->deleteLater();
    m_shellItem = nullptr;
    m_toplevel.clear();
    m_xdgSurface.clear();
    setReady(false);
    setTransport(QStringLiteral("none"));
    qCWarning(lcSurfaceWayland) << m_token.left(8) << errorCode;
    emit lost(errorCode);
}

void WaylandSurfaceBackend::geometryChanged(const QSizeF& logicalSize)
{
    if (m_shellItem) m_shellItem->setSize(logicalSize);
    sendConfigure();
}

void WaylandSurfaceBackend::visibilityChanged(bool)
{
    updateOutputGate();
}

void WaylandSurfaceBackend::sendConfigure()
{
    if (!m_toplevel) return;
    const QSize logical(qRound(m_item->width()), qRound(m_item->height()));
    if (logical.isEmpty()) return;
    // Logical pixels; the client multiplies by the output scale itself.
    m_toplevel->sendConfigure(logical, QList<int>{});
}

void WaylandSurfaceBackend::updateOutputGate()
{
    if (!m_shellItem || !m_compositor) return;
    // An invisible item leaves its output: the surface then gets no frame
    // callbacks and the client idles (RFC-5 rule 1). The hidden-tab case is
    // already covered by the tab's window not rendering at all.
    QWaylandOutput* output = m_item->isVisible() && m_item->window()
                                 ? m_compositor->outputFor(m_item->window()) : nullptr;
    m_shellItem->setOutput(output);
}
