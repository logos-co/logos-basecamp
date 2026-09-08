#pragma once

#include <QObject>

class QCoreApplication;
#include <QStringList>

// ── LinkUrlInbox ─────────────────────────────────────────────────────────────
//
// Where every inbound URL lands, from whichever of the three sources produced
// it: this process's own argv (cold start on Linux/Windows), a macOS
// QFileOpenEvent, or a secondary instance over the single-instance socket.
//
// A PROCESS-WIDE OBJECT, because the producers exist before the consumer does.
// The guard and the event filter have to be installed in main() before
// logos_core_start() — a cold-start QFileOpenEvent arrives during launch and is
// simply lost if the filter is not up — whereas LinkRequestCoordinator cannot
// exist until MainUIBackend has built the broker and registry, seconds later.
// Window keeps MainUIBackend private and main() has no route to it, so the
// alternative was threading a pointer through the shell for one signal.
//
// The queue is the cold-start stash. A URL posted before anything is listening
// stays here until drained, which is what makes the first click after a link
// launches the app behave the same as one clicked into a running app.
class LinkUrlInbox : public QObject {
    Q_OBJECT
public:
    static LinkUrlInbox& instance();

    // Queue a URL and announce it. Safe before any consumer exists.
    void post(const QString& url);

    // Take everything queued so far, clearing the queue.
    QStringList takeAll();

    bool isEmpty() const;

    // macOS delivery, installed on the application object.
    static void installEventFilter(QCoreApplication* app);

signals:
    // Emitted after the URL is queued, so a consumer woken by this always finds
    // it in takeAll() rather than racing the append.
    void urlPosted();

private:
    LinkUrlInbox() = default;

    // A page can navigate repeatedly. The queue is bounded so a flood cannot
    // grow it without limit; the coordinator caps in-flight requests
    // separately, and both drop loudly rather than silently.
    static constexpr int kMaxQueued = 8;

    QStringList m_queued;
};
