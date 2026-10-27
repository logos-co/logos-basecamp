#pragma once

// RFC-4 backend of Logos.Surface: shows the xdg toplevel the nested
// SurfaceCompositor admitted under the module's token, through a
// QWaylandQuickShellSurfaceItem child of the item. Input goes to that child;
// the item paints nothing once the client's first buffer is current.
// Linux only (compiled under LOGOS_HAVE_WAYLAND_COMPOSITOR).

#include "SurfaceBackend.h"

#include <QPointer>

class QTimer;
class QWaylandQuickShellSurfaceItem;
class QWaylandXdgSurface;
class QWaylandXdgToplevel;
class SurfaceCompositor;

class WaylandSurfaceBackend : public SurfaceBackend {
    Q_OBJECT
public:
    explicit WaylandSurfaceBackend(QQuickItem* item);
    ~WaylandSurfaceBackend() override;

    QString family() const override { return QStringLiteral("wayland"); }
    bool bind(const QString& module, QString* errorCode) override;
    void unbind() override;
    void geometryChanged(const QSizeF& logicalSize) override;
    void visibilityChanged(bool visible) override;

private:
    void attach();
    void detach(const QString& errorCode);
    void sendConfigure();
    void updateOutputGate();
    void ensureEntered();

    QString m_token;
    QPointer<SurfaceCompositor> m_compositor;
    QWaylandQuickShellSurfaceItem* m_shellItem = nullptr;
    QPointer<QWaylandXdgToplevel> m_toplevel;
    QPointer<QWaylandXdgSurface> m_xdgSurface;
    QTimer* m_enterTimer = nullptr;
    int m_enterAttempts = 0;
};
