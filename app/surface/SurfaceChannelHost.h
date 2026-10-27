#pragma once

// The host side of RFC-7 in Basecamp: one SurfaceHostSession per view_surface
// module spawned on the channel transport, keyed by module name exactly as
// SurfaceCompositor keys its admission table (RFC-4 §2). PluginLoader opens a
// session before it spawns (the server name goes on ui-host's command line),
// registers the token and pid right after spawn() returns, and forgets the
// module before stop(). The Logos.Surface item of that module finds its
// session here through ChannelSurfaceBackend.
//
// Process-wide and lazily created, like the compositor; works on every OS
// (shm) and never starts anything a module could reach.

#include <QHash>
#include <QObject>
#include <QString>


namespace logos::surface { class SurfaceHostSession; }

class SurfaceChannelHost : public QObject {
    Q_OBJECT
public:
    static SurfaceChannelHost* instance();
    static SurfaceChannelHost* existing();

    ~SurfaceChannelHost() override;

    // Opens a listening session for a module about to be spawned. Returns
    // the server name for ViewSpawnOptions::surfaceSocket, or empty when the
    // server could not be created. A previous session for the same module is
    // closed first (a reload).
    QString open(const QString& module);
    // Admission: the token the spawn was minted with and the child's pid.
    void expect(const QString& module, const QString& token, qint64 pid);
    // Before ViewModuleHost::stop(): the disconnect that follows is then not
    // reported as a lost surface, and the slot ring is freed.
    void forget(const QString& module);

    logos::surface::SurfaceHostSession* sessionFor(const QString& module) const;
    bool hasSession(const QString& module) const { return m_sessions.contains(module); }

    // LOGOS_SURFACE_TRANSPORT=shm: every session negotiates shm only.
    void setForcedShm(bool forced) { m_forcedShm = forced; }

signals:
    void sessionOpened(const QString& module);
    void sessionClosed(const QString& module);

private:
    explicit SurfaceChannelHost(QObject* parent);

    // QObject-parented to this host; forget() hands one to deleteLater().
    QHash<QString, logos::surface::SurfaceHostSession*> m_sessions;
    bool m_forcedShm = false;
};
