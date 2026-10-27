#pragma once

// RFC-7 backend of Logos.Surface: samples the newest ready slot of the
// module's SurfaceHostSession with a QSGSimpleTextureNode and forwards the
// item's input events to the view as protocol messages. Compiled on every
// OS; the slot kind (shm / d3d11 / iosurface) is the session's business.
//
// Threading: updatePaintNode() runs on the render thread with the GUI thread
// blocked and is the only place takeReadyForDisplay() is called; the window's
// afterRendering (render thread) is forwarded to the session with a queued
// connection so Release follows the frame that showed the successor.
//
// Pacing is the session's (RFC-7 §5): the view holds a standing credit and
// renders one frame per host paint. basecamp.surface.channel.item.debug=true
// prints a one-line-per-second summary (frames arrived / shown / host swaps /
// arrival→paint wait) to see it live.

#include "SurfaceBackend.h"

#include <SurfaceHostSession.h>

#include <QElapsedTimer>
#include <QPointer>

class QQuickWindow;
class QSGTexture;

class ChannelSurfaceBackend : public SurfaceBackend {
    Q_OBJECT
public:
    explicit ChannelSurfaceBackend(QQuickItem* item);
    ~ChannelSurfaceBackend() override;

    QString family() const override { return QStringLiteral("channel"); }
    bool bind(const QString& module, QString* errorCode) override;
    void unbind() override;
    void geometryChanged(const QSizeF& logicalSize) override;
    void visibilityChanged(bool visible) override;

    QSGNode* updatePaintNode(QSGNode* oldNode) override;
    bool paintsItself() const override { return true; }
    bool wantsItemInput() const override { return true; }
    void forwardEvent(QEvent* event) override;

    logos::surface::SurfaceHostSession* session() const { return m_session; }

private:
    void configure();
    void attachWindow(QQuickWindow* window);

    QString m_module;
    QPointer<logos::surface::SurfaceHostSession> m_session;
    QPointer<QQuickWindow> m_window;
    logos::surface::HostSlot* m_shownSlot = nullptr;   // render thread only
    bool m_hasFocus = false;

    // Pacing statistics, printed once a second when
    // basecamp.surface.channel.item.debug=true: frames that arrived, paints
    // that showed a new one, host frames swapped, and the arrival→paint wait.
    struct Pacing {
        QElapsedTimer clock;
        qint64 lastReportNs = 0;
        qint64 lastArrivalNs = 0;      // GUI thread
        qint64 arrivalWaitNs = 0;      // sum of arrival→updatePaintNode (render thread reads lastArrivalNs)
        int arrived = 0, shown = 0, swapped = 0;
    } m_pacing;
    void reportPacing();
};
