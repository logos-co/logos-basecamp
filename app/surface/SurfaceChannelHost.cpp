#include "SurfaceChannelHost.h"

#include <SurfaceHostSession.h>

#include <QCoreApplication>
#include <QLoggingCategory>

#include <utility>

Q_LOGGING_CATEGORY(lcSurfaceChannelHost, "basecamp.surface.channel")

using logos::surface::SurfaceHostSession;

namespace {
SurfaceChannelHost* g_instance = nullptr;
}

SurfaceChannelHost* SurfaceChannelHost::instance()
{
    if (!g_instance) g_instance = new SurfaceChannelHost(QCoreApplication::instance());
    return g_instance;
}

SurfaceChannelHost* SurfaceChannelHost::existing()
{
    return g_instance;
}

SurfaceChannelHost::SurfaceChannelHost(QObject* parent) : QObject(parent)
{
    if (qEnvironmentVariable("LOGOS_SURFACE_TRANSPORT") == QLatin1String("shm")) m_forcedShm = true;
}

SurfaceChannelHost::~SurfaceChannelHost()
{
    for (SurfaceHostSession* s : std::as_const(m_sessions)) s->close();
    m_sessions.clear();
    if (g_instance == this) g_instance = nullptr;
}

QString SurfaceChannelHost::open(const QString& module)
{
    if (module.isEmpty()) return {};
    forget(module);
    auto* session = new SurfaceHostSession(this);
    if (m_forcedShm) session->setForcedKind(logos::surface::SlotKindShm);
    if (!session->listen()) {
        qCWarning(lcSurfaceChannelHost) << "could not open a channel session for" << module;
        delete session;
        return {};
    }
    const QString name = session->serverName();
    m_sessions.insert(module, session);
    qCInfo(lcSurfaceChannelHost) << "session for" << module << "listening on" << name;
    emit sessionOpened(module);
    return name;
}

void SurfaceChannelHost::expect(const QString& module, const QString& token, qint64 pid)
{
    if (SurfaceHostSession* s = sessionFor(module)) s->expect(token, pid);
}

void SurfaceChannelHost::forget(const QString& module)
{
    auto it = m_sessions.find(module);
    if (it == m_sessions.end()) return;
    it.value()->close();
    // The item's backend may still hold a pointer: let the deletion happen
    // after the current event, once it has seen closed().
    SurfaceHostSession* raw = it.value();
    raw->deleteLater();
    m_sessions.erase(it);
    emit sessionClosed(module);
}

SurfaceHostSession* SurfaceChannelHost::sessionFor(const QString& module) const
{
    auto it = m_sessions.find(module);
    return it == m_sessions.end() ? nullptr : it.value();
}
