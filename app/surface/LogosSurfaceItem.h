#pragma once

// `Logos.Surface` (RFC-3): the one QML type through which a sandboxed ui_qml
// view shows the surface its own backend renders out of process. Host code,
// compiled into Basecamp and registered as the QML module Logos.Surface; a
// module cannot ship its own (SharedLogosModules.h lists it as vetted, so a
// plugin-tree Logos/Surface is redirected away).
//
//     import Logos.Surface
//     Surface { anchors.fill: parent; module: "maps_view"; session: "basecamp/main" }
//
// The item never learns a token from QML. PluginLoader tells the sandboxed
// engine which module it hosts (bindEngine); the item compares that with
// `module`, then hands the work to the SurfaceBackend for the transport that
// module was spawned with (RFC-6): WaylandSurfaceBackend shows the toplevel
// the nested compositor admitted under the module's token (Linux),
// ChannelSurfaceBackend samples the module's SurfaceHostSession slot ring
// (every OS). The only new capability a view gains is displaying a buffer
// its own backend produced.
//
// Without a transport (the module was not spawned with one, or
// LOGOS_SURFACE_TRANSPORT=none) the item paints a placeholder, reports
// `transport: "none"` and emits error("noCompositor") / error("denied").

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class SurfaceBackend;

class LogosSurfaceItem : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(Surface)
    Q_PROPERTY(QString module READ module WRITE setModule NOTIFY moduleChanged)
    Q_PROPERTY(QString session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QString viewId READ viewId NOTIFY viewIdChanged)
    Q_PROPERTY(bool ready READ isReady NOTIFY readyChanged)
    Q_PROPERTY(QString transport READ transport NOTIFY transportChanged)
    Q_PROPERTY(QColor placeholderColor READ placeholderColor WRITE setPlaceholderColor
               NOTIFY placeholderColorChanged)

public:
    explicit LogosSurfaceItem(QQuickItem* parent = nullptr);
    ~LogosSurfaceItem() override;

    // Host side: record on a sandboxed engine which module's view it runs.
    // A dynamic property on the QQmlEngine object, which QML cannot reach.
    static void bindEngine(QQmlEngine* engine, const QString& module);
    static QString boundModule(QQmlEngine* engine);

    QString module() const { return m_module; }
    void setModule(const QString& module);
    QString session() const { return m_session; }
    void setSession(const QString& session);
    QString viewId() const { return m_viewId; }
    bool isReady() const { return m_ready; }
    QString transport() const { return m_transport; }
    QColor placeholderColor() const { return m_placeholderColor; }
    void setPlaceholderColor(const QColor& color);

    // Test seam: the backend in use, if any.
    SurfaceBackend* backend() const { return m_backend; }

signals:
    void moduleChanged();
    void sessionChanged();
    void viewIdChanged();
    void readyChanged();
    void transportChanged();
    void placeholderColorChanged();
    // noCompositor | notGuiModule | tokenMismatch | denied | clientExited |
    // protocolError | allocationFailed
    void error(const QString& code);

protected:
    void componentComplete() override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    bool event(QEvent* event) override;

private:
    void tryBind();
    void setReady(bool ready);
    void setTransport(const QString& transport);
    void fail(const QString& code);
    SurfaceBackend* makeBackend(const QString& family);

    QString m_module;
    QString m_session;
    QString m_viewId;
    QString m_transport = QStringLiteral("none");
    bool m_ready = false;
    bool m_failed = false;
    QColor m_placeholderColor = QColor(QStringLiteral("#1e1e1e"));

    SurfaceBackend* m_backend = nullptr;
    QSGNode* m_placeholderNode = nullptr;   // render thread only; identity, never dereferenced
    static int s_nextViewSerial;
};
