#pragma once

// The transport behind a Logos.Surface item (RFC-6). The item keeps its QML
// API and the sandbox checks; a backend owns everything transport-specific:
//
//   WaylandSurfaceBackend   RFC-4: the toplevel the nested compositor admitted
//                           under the module's token, shown by a
//                           QWaylandQuickShellSurfaceItem (Linux only)
//   ChannelSurfaceBackend   RFC-7: the SurfaceHostSession opened for the
//                           module's spawn, its ready slot sampled by a
//                           QSGSimpleTextureNode, input forwarded as messages
//
// A backend is created by SurfaceTransportSelector for the family the module
// was spawned with and lives as long as the item does.

#include <QObject>
#include <QQuickItem>
#include <QSizeF>
#include <QString>

class QEvent;
class QSGNode;

class SurfaceBackend : public QObject {
    Q_OBJECT
public:
    explicit SurfaceBackend(QQuickItem* item) : QObject(item), m_item(item) {}
    ~SurfaceBackend() override;

    // RFC-6 §3 family name: "wayland" | "channel".
    virtual QString family() const = 0;

    // Attaches the module's surface. False with `errorCode` set means this
    // backend cannot serve the module at all (noCompositor, notGuiModule);
    // true means attached, or waiting for the surface to appear (the backend
    // emits readyChanged(true) when it does).
    virtual bool bind(const QString& module, QString* errorCode) = 0;
    virtual void unbind() = 0;

    // Host-authoritative geometry and visibility (RFC-6 §1 c, RFC-5 rule 1).
    virtual void geometryChanged(const QSizeF& logicalSize) = 0;
    virtual void visibilityChanged(bool visible) = 0;

    // Scene graph. A backend that paints through a child item returns
    // nullptr and leaves `oldNode` alone; one that paints itself owns the
    // returned node. Render thread, GUI thread blocked.
    virtual QSGNode* updatePaintNode(QSGNode* oldNode) { Q_UNUSED(oldNode); return nullptr; }
    virtual bool paintsItself() const { return false; }

    // Input. A backend that wants the item's events (rather than a child
    // item's) returns true here and gets them through forwardEvent().
    virtual bool wantsItemInput() const { return false; }
    virtual void forwardEvent(QEvent* event) { Q_UNUSED(event); }

    bool isReady() const { return m_ready; }
    QString transport() const { return m_transport; }

signals:
    void readyChanged(bool ready);
    void transportChanged(const QString& transport);
    // clientExited | protocolError | allocationFailed
    void lost(const QString& code);

protected:
    void setReady(bool ready)
    {
        if (m_ready == ready) return;
        m_ready = ready;
        emit readyChanged(ready);
    }
    void setTransport(const QString& transport)
    {
        if (m_transport == transport) return;
        m_transport = transport;
        emit transportChanged(transport);
    }

    QQuickItem* m_item;

private:
    bool m_ready = false;
    QString m_transport = QStringLiteral("none");
};
