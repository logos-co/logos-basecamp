#include "LogosSurfaceItem.h"

#include "SurfaceBackend.h"
#include "SurfaceTransportSelector.h"

#include <QEvent>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGSimpleRectNode>

#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
#include "WaylandSurfaceBackend.h"
#endif
#ifdef LOGOS_HAVE_SURFACE_CHANNEL
#include "ChannelSurfaceBackend.h"
#include "SurfaceChannelHost.h"
#endif

Q_LOGGING_CATEGORY(lcSurfaceItem, "basecamp.surface.item")

namespace {
constexpr const char* kEngineModuleProperty = "logos.surface.module";
}

int LogosSurfaceItem::s_nextViewSerial = 1;

LogosSurfaceItem::LogosSurfaceItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // Input stays off until a backend that wants the item's events binds;
    // the Wayland backend's child item takes input itself.
    setAcceptedMouseButtons(Qt::NoButton);
}

LogosSurfaceItem::~LogosSurfaceItem()
{
    if (m_backend) m_backend->unbind();
}

void LogosSurfaceItem::bindEngine(QQmlEngine* engine, const QString& module)
{
    if (engine) engine->setProperty(kEngineModuleProperty, module);
}

QString LogosSurfaceItem::boundModule(QQmlEngine* engine)
{
    return engine ? engine->property(kEngineModuleProperty).toString() : QString();
}

void LogosSurfaceItem::setModule(const QString& module)
{
    if (m_module == module) return;
    m_module = module;
    emit moduleChanged();
    tryBind();
}

void LogosSurfaceItem::setSession(const QString& session)
{
    if (m_session == session) return;
    m_session = session;
    emit sessionChanged();
}

void LogosSurfaceItem::setPlaceholderColor(const QColor& color)
{
    if (m_placeholderColor == color) return;
    m_placeholderColor = color;
    emit placeholderColorChanged();
    update();
}

void LogosSurfaceItem::componentComplete()
{
    QQuickItem::componentComplete();
    tryBind();
}

void LogosSurfaceItem::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange) {
        tryBind();
        if (m_backend) m_backend->visibilityChanged(isVisible() && window());
    } else if (change == ItemVisibleHasChanged) {
        if (m_backend) m_backend->visibilityChanged(value.boolValue && window());
    }
}

void LogosSurfaceItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size()) return;
    if (m_backend) m_backend->geometryChanged(newGeometry.size());
    update();
}

QSGNode* LogosSurfaceItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    if (m_backend && m_backend->paintsItself()) {
        // The backend's node, or the placeholder while it has nothing yet.
        // The backend owns whatever node it is handed (it reuses or deletes
        // it); the placeholder is ours and is never handed over.
        const bool oldIsPlaceholder = oldNode && oldNode == m_placeholderNode;
        QSGNode* node = m_backend->updatePaintNode(oldIsPlaceholder ? nullptr : oldNode);
        if (node) {
            if (oldIsPlaceholder) delete oldNode;
            m_placeholderNode = nullptr;
            return node;
        }
        auto* rect = oldIsPlaceholder ? static_cast<QSGSimpleRectNode*>(oldNode)
                                      : new QSGSimpleRectNode;
        m_placeholderNode = rect;
        rect->setRect(boundingRect());
        rect->setColor(m_placeholderColor);
        return rect;
    }
    // Once the client's buffer is on screen the shell item paints; the
    // placeholder behind it would only cost fill rate.
    if (m_ready) {
        delete oldNode;
        return nullptr;
    }
    auto* node = static_cast<QSGSimpleRectNode*>(oldNode);
    if (!node) node = new QSGSimpleRectNode;
    node->setRect(boundingRect());
    node->setColor(m_placeholderColor);
    return node;
}

bool LogosSurfaceItem::event(QEvent* event)
{
    if (m_backend && m_backend->wantsItemInput()) {
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
        case QEvent::HoverLeave:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            if (event->type() == QEvent::MouseButtonPress) forceActiveFocus(Qt::MouseFocusReason);
            m_backend->forwardEvent(event);
            if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
                return QQuickItem::event(event);
            return true;
        default:
            break;
        }
    }
    return QQuickItem::event(event);
}

void LogosSurfaceItem::setReady(bool ready)
{
    if (m_ready == ready) return;
    m_ready = ready;
    emit readyChanged();
    update();
}

void LogosSurfaceItem::setTransport(const QString& transport)
{
    if (m_transport == transport) return;
    m_transport = transport;
    emit transportChanged();
}

void LogosSurfaceItem::fail(const QString& code)
{
    if (m_failed) return;
    m_failed = true;
    setTransport(QStringLiteral("none"));
    qCWarning(lcSurfaceItem) << "Logos.Surface for" << m_module << ":" << code;
    emit error(code);
}

SurfaceBackend* LogosSurfaceItem::makeBackend(const QString& family)
{
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
    if (family == QLatin1String("wayland")) return new WaylandSurfaceBackend(this);
#endif
#ifdef LOGOS_HAVE_SURFACE_CHANNEL
    if (family == QLatin1String("channel")) return new ChannelSurfaceBackend(this);
#endif
    return nullptr;
}

void LogosSurfaceItem::tryBind()
{
    if (m_failed || m_backend) return;
    if (!isComponentComplete() || !window() || m_module.isEmpty()) return;

    // A view may only show its own backend. The sandbox recorded on the engine
    // which module this view belongs to; nothing the QML can set changes it.
    const QString own = boundModule(qmlEngine(this));
    if (own.isEmpty() || own != m_module) {
        fail(QStringLiteral("tokenMismatch"));
        return;
    }

    // The family the module was spawned with (PluginLoader recorded it). A
    // module nobody recorded falls back to whatever this host has running, so
    // a test that only drives the compositor or only the channel host works:
    // a channel session for the module if one exists, else a compositor.
    QString family = SurfaceTransportSelector::familyFor(m_module);
    if (family.isEmpty()) {
#ifdef LOGOS_HAVE_SURFACE_CHANNEL
        if (SurfaceChannelHost* host = SurfaceChannelHost::existing(); host && host->sessionFor(m_module))
            family = QStringLiteral("channel");
#endif
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
        if (family.isEmpty() && SurfaceTransportSelector::waylandAvailable()) family = QStringLiteral("wayland");
#endif
#ifdef LOGOS_HAVE_SURFACE_CHANNEL
        if (family.isEmpty()) family = QStringLiteral("channel");
#endif
    }
    SurfaceBackend* backend = makeBackend(family);
    if (!backend) {
        fail(family.isEmpty() ? QStringLiteral("denied") : QStringLiteral("noCompositor"));
        return;
    }

    QString code;
    if (!backend->bind(m_module, &code)) {
        delete backend;
        fail(code);
        return;
    }
    m_backend = backend;
    m_viewId = QStringLiteral("%1/%2/%3").arg(m_module, m_session).arg(s_nextViewSerial++);
    emit viewIdChanged();

    connect(backend, &SurfaceBackend::readyChanged, this, &LogosSurfaceItem::setReady);
    connect(backend, &SurfaceBackend::transportChanged, this, &LogosSurfaceItem::setTransport);
    connect(backend, &SurfaceBackend::lost, this, [this](const QString& code) {
        qCWarning(lcSurfaceItem) << m_viewId << code;
        emit error(code);
    });
    if (backend->wantsItemInput()) {
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
        setAcceptTouchEvents(true);
        setFlag(ItemIsFocusScope, false);
        setFocus(true);
    }
    setReady(backend->isReady());
    setTransport(backend->transport());
    backend->geometryChanged(size());
    backend->visibilityChanged(isVisible() && window());
    qCInfo(lcSurfaceItem) << "bound" << m_viewId << "via" << family;
}
