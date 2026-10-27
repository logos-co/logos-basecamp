#include "ChannelSurfaceBackend.h"

#include "SurfaceChannelHost.h"

#include <SlotStorage.h>
#include <SurfaceHostSession.h>

#include <QHoverEvent>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QTouchEvent>
#include <QWheelEvent>

Q_LOGGING_CATEGORY(lcSurfaceChannel, "basecamp.surface.channel.item")

using namespace logos::surface;

namespace {

// A texture node that also owns the slot it was created from, so the render
// thread can tell when a new slot has to replace it.
class SlotTextureNode : public QSGSimpleTextureNode {
public:
    HostSlot* slot = nullptr;
};

} // namespace

ChannelSurfaceBackend::ChannelSurfaceBackend(QQuickItem* item)
    : SurfaceBackend(item)
{
}

ChannelSurfaceBackend::~ChannelSurfaceBackend() = default;

bool ChannelSurfaceBackend::bind(const QString& module, QString* errorCode)
{
    if (m_session) return true;
    SurfaceChannelHost* host = SurfaceChannelHost::existing();
    SurfaceHostSession* session = host ? host->sessionFor(module) : nullptr;
    if (!session) {
        *errorCode = QStringLiteral("notGuiModule");
        return false;
    }
    m_module = module;
    m_session = session;

    connect(session, &SurfaceHostSession::frameReady, this, [this] {
        if (!m_pacing.clock.isValid()) m_pacing.clock.start();
        m_pacing.lastArrivalNs = m_pacing.clock.nsecsElapsed();
        ++m_pacing.arrived;
        if (m_item) m_item->update();
    });
    connect(session, &SurfaceHostSession::transportChanged, this, [this] {
        if (m_session) setTransport(m_session->isConnected() ? m_session->transport() : QStringLiteral("none"));
    });
    connect(session, &SurfaceHostSession::connected, this, [this] {
        if (m_session) setTransport(m_session->transport());
        configure();
    });
    connect(session, &SurfaceHostSession::error, this, [this](const QString& code) {
        // The last frame stays on screen (the displayed slot survives the
        // disconnect); the item is told so it can react.
        setReady(false);
        setTransport(QStringLiteral("none"));
        emit lost(code);
    });
    connect(session, &SurfaceHostSession::closed, this, [this] {
        setReady(false);
        setTransport(QStringLiteral("none"));
    });
    connect(host, &SurfaceChannelHost::sessionClosed, this, [this](const QString& m) {
        if (m == m_module) {
            m_session.clear();
            setReady(false);
            setTransport(QStringLiteral("none"));
        }
    });

    attachWindow(m_item->window());
    if (session->isConnected()) setTransport(session->transport());
    configure();
    return true;
}

void ChannelSurfaceBackend::attachWindow(QQuickWindow* window)
{
    if (m_window == window) return;
    if (m_window) m_window->disconnect(this);
    m_window = window;
    if (!window) return;
    if (m_session) m_session->setHostWindow(window);
    // Render thread → GUI thread: the frame that painted the successor has
    // been issued, so the slot it replaced can go back to the view (RFC-7 §5).
    connect(window, &QQuickWindow::afterRendering, this, [this] {
        ++m_pacing.swapped;
        reportPacing();
        if (m_session) m_session->frameSwapped();
    }, Qt::QueuedConnection);
    connect(window, &QQuickWindow::screenChanged, this, [this] { configure(); });
}

void ChannelSurfaceBackend::reportPacing()
{
    if (!lcSurfaceChannel().isDebugEnabled() || !m_pacing.clock.isValid()) return;
    const qint64 now = m_pacing.clock.nsecsElapsed();
    if (now - m_pacing.lastReportNs < 1000000000LL) return;
    const double secs = double(now - m_pacing.lastReportNs) / 1e9;
    qCDebug(lcSurfaceChannel, "pacing %s: %.0f frames/s arrived, %.0f/s shown, %.0f/s host swaps, arrival->paint %.1f ms",
            qPrintable(m_module), m_pacing.arrived / secs, m_pacing.shown / secs, m_pacing.swapped / secs,
            m_pacing.shown ? double(m_pacing.arrivalWaitNs) / m_pacing.shown / 1e6 : 0.0);
    m_pacing.lastReportNs = now;
    m_pacing.arrived = m_pacing.shown = m_pacing.swapped = 0;
    m_pacing.arrivalWaitNs = 0;
}

void ChannelSurfaceBackend::unbind()
{
    if (m_session) {
        m_session->disconnect(this);
        m_session->setVisible(false);
    }
    if (m_window) m_window->disconnect(this);
    if (SurfaceChannelHost* host = SurfaceChannelHost::existing()) host->disconnect(this);
    m_session.clear();
    m_window.clear();
    setReady(false);
    setTransport(QStringLiteral("none"));
}

void ChannelSurfaceBackend::configure()
{
    if (!m_session || !m_item) return;
    QQuickWindow* window = m_item->window();
    if (window != m_window) attachWindow(window);
    const QSize logical(qRound(m_item->width()), qRound(m_item->height()));
    if (logical.isEmpty()) return;
    const qreal dpr = window ? window->effectiveDevicePixelRatio() : 1.0;
    m_session->configure(logical, dpr, m_item->isVisible() && window != nullptr);
}

void ChannelSurfaceBackend::geometryChanged(const QSizeF&)
{
    configure();
}

void ChannelSurfaceBackend::visibilityChanged(bool visible)
{
    if (!m_session) return;
    if (m_item->window() != m_window) attachWindow(m_item->window());
    m_session->setVisible(visible && m_item->window() != nullptr);
    if (visible) configure();
}

QSGNode* ChannelSurfaceBackend::updatePaintNode(QSGNode* oldNode)
{
    auto* node = static_cast<SlotTextureNode*>(oldNode);
    if (!m_session) {
        delete node;
        m_shownSlot = nullptr;
        return nullptr;
    }
    QQuickWindow* window = m_item->window();
    HostSlot* fresh = m_session->takeReadyForDisplay();
    HostSlot* slot = fresh ? fresh : m_session->displayedSlot();
    if (!slot || !window) {
        delete node;
        m_shownSlot = nullptr;
        return nullptr;
    }
    if (!node) {
        node = new SlotTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }
    if (node->slot != slot || fresh) {
        if (fresh && m_pacing.clock.isValid()) {
            ++m_pacing.shown;
            m_pacing.arrivalWaitNs += m_pacing.clock.nsecsElapsed() - m_pacing.lastArrivalNs;
        }
        QSGTexture* texture = slot->createTexture(window);
        if (!texture) {
            delete node;
            m_shownSlot = nullptr;
            return nullptr;
        }
        node->setTexture(texture);
        node->slot = slot;
        m_shownSlot = slot;
        // The pixels are in the texture now: the item is showing the module.
        if (!isReady())
            QMetaObject::invokeMethod(this, [this] { setReady(true); }, Qt::QueuedConnection);
    }
    node->setRect(m_item->boundingRect());
    return node;
}

// ── Input (RFC-7 §6) ───────────────────────────────────────────────────────

void ChannelSurfaceBackend::forwardEvent(QEvent* event)
{
    if (!m_session || !m_session->isConnected()) return;
    Input in;
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: {
        auto* e = static_cast<QMouseEvent*>(event);
        in.kind = event->type() == QEvent::MouseMove ? InputKind::MouseMove
                : event->type() == QEvent::MouseButtonPress ? InputKind::MousePress
                : event->type() == QEvent::MouseButtonRelease ? InputKind::MouseRelease
                                                               : InputKind::MouseDoubleClick;
        in.mouse.x = e->position().x(); in.mouse.y = e->position().y();
        in.mouse.gx = e->position().x(); in.mouse.gy = e->position().y();   // the view has no screen
        in.mouse.button = uint32_t(e->button());
        in.mouse.buttons = uint32_t(e->buttons());
        in.mouse.modifiers = uint32_t(e->modifiers());
        in.mouse.timestamp = e->timestamp();
        event->accept();
        break;
    }
    case QEvent::HoverEnter:
    case QEvent::HoverMove: {
        auto* e = static_cast<QHoverEvent*>(event);
        in.kind = InputKind::MouseMove;
        in.mouse.x = e->position().x(); in.mouse.y = e->position().y();
        in.mouse.gx = in.mouse.x; in.mouse.gy = in.mouse.y;
        in.mouse.modifiers = uint32_t(e->modifiers());
        in.mouse.timestamp = e->timestamp();
        break;
    }
    case QEvent::HoverLeave:
        in.kind = InputKind::Leave;
        break;
    case QEvent::Wheel: {
        auto* e = static_cast<QWheelEvent*>(event);
        in.kind = InputKind::Wheel;
        in.wheel.x = e->position().x(); in.wheel.y = e->position().y();
        in.wheel.gx = in.wheel.x; in.wheel.gy = in.wheel.y;
        in.wheel.pixelDx = e->pixelDelta().x(); in.wheel.pixelDy = e->pixelDelta().y();
        in.wheel.angleDx = e->angleDelta().x(); in.wheel.angleDy = e->angleDelta().y();
        in.wheel.buttons = uint32_t(e->buttons());
        in.wheel.modifiers = uint32_t(e->modifiers());
        in.wheel.phase = uint32_t(e->phase());
        in.wheel.inverted = e->inverted() ? 1 : 0;
        in.wheel.timestamp = e->timestamp();
        event->accept();
        break;
    }
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        auto* e = static_cast<QTouchEvent*>(event);
        in.kind = InputKind::Touch;
        in.touch.type = uint32_t(event->type());
        in.touch.modifiers = uint32_t(e->modifiers());
        in.touch.timestamp = e->timestamp();
        for (const QEventPoint& p : e->points()) {
            if (in.touch.points.size() >= kMaxTouchPoints) break;
            TouchPointInput tp;
            tp.id = p.id();
            tp.state = uint32_t(p.state());
            // Item-local; the item's own position in the window is the
            // host's business, not the view's.
            const QPointF local = m_item->mapFromScene(p.scenePosition());
            tp.x = local.x(); tp.y = local.y(); tp.gx = local.x(); tp.gy = local.y();
            tp.pressure = p.pressure();
            in.touch.points.append(tp);
        }
        event->accept();
        break;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto* e = static_cast<QKeyEvent*>(event);
        in.kind = event->type() == QEvent::KeyPress ? InputKind::KeyPress : InputKind::KeyRelease;
        in.key.key = e->key();
        in.key.modifiers = uint32_t(e->modifiers());
        in.key.nativeScanCode = e->nativeScanCode();
        in.key.nativeVirtualKey = e->nativeVirtualKey();
        in.key.nativeModifiers = e->nativeModifiers();
        in.key.autoRepeat = e->isAutoRepeat() ? 1 : 0;
        in.key.count = uint16_t(e->count());
        in.key.text = e->text().left(kMaxKeyText);
        event->accept();
        break;
    }
    case QEvent::FocusIn:
        in.kind = InputKind::FocusIn;
        m_hasFocus = true;
        break;
    case QEvent::FocusOut:
        in.kind = InputKind::FocusOut;
        m_hasFocus = false;
        break;
    default:
        return;
    }
    m_session->sendInput(in);
}
