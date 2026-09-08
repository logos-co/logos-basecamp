#include "LinkRequestCoordinator.h"

#include "IntentBroker.h"
#include "IntentRegistry.h"
#include "ShellIntentEndpoint.h"
#include "LinkUrl.h"
#include "LinkUrlInbox.h"
#include "LogosIntent.h"

#include <QDebug>

LinkRequestCoordinator::LinkRequestCoordinator(IntentBroker* broker,
                                               IntentRegistry* registry,
                                               QObject* parent)
    : QObject(parent)
    , m_broker(broker)
    , m_registry(registry)
{
    // No DeliverFn: a link is a requester only. Nothing lists this name in
    // `provides`, so the broker has no path back here, and the null callable
    // makes deliverRequest report 0 handlers if one were ever attempted.
    //
    // The result has nowhere else to go — a browser gets no channel back, by
    // construction — so it reaches the shell's UI and the log. That is also why
    // no error code on this path can leak anything.
    m_endpoint = std::make_unique<ShellIntentEndpoint>(
        ShellIntentEndpoint::DeliverFn{},
        [this](const QString& requestId, const QVariantMap& envelope) {
            onResult(requestId, envelope);
        });

    if (m_broker)
        m_broker->registerEndpoint(requesterName(), m_endpoint.get());

    m_parkDeadline.setSingleShot(true);
    connect(&m_parkDeadline, &QTimer::timeout, this, [this]() {
        if (m_ready || LinkUrlInbox::instance().isEmpty())
            return;
        const QStringList abandoned = LinkUrlInbox::instance().takeAll();
        fail(QStringLiteral("the app did not finish starting in time to open %1")
                 .arg(abandoned.join(QStringLiteral(", "))));
    });

    connect(&LinkUrlInbox::instance(), &LinkUrlInbox::urlPosted,
            this, &LinkRequestCoordinator::drainInbox);

    // A URL may already be queued: on a cold start the guard and the macOS
    // event filter both run in main() long before this object exists.
    if (!LinkUrlInbox::instance().isEmpty())
        m_parkDeadline.start(m_parkDeadlineMs);
}

LinkRequestCoordinator::~LinkRequestCoordinator()
{
    // Only if the broker is still alive. It usually is NOT: children are deleted
    // in construction order, so MainUIBackend frees the broker several children
    // before it gets to us. m_broker is a QPointer precisely so this reads as a
    // null check rather than a use-after-free.
    //
    // When the coordinator IS destroyed first, unregistering matters: the broker
    // keeps raw endpoint pointers and a stale one is a use-after-free on its
    // next sweep.
    if (m_broker && m_endpoint)
        m_broker->unregisterEndpoint(m_endpoint.get());
}

QString LinkRequestCoordinator::requesterName()
{
    // Not "main_ui". A link must face the chooser, and being the shell is what
    // skips it. IntentRegistry refuses this name from any disk record for the
    // matching reason: a package answering to it would inherit the
    // web-reachable `uses` set.
    return QStringLiteral("logos_link");
}

void LinkRequestCoordinator::setRaiseHandler(std::function<void()> raise)
{
    m_raise = std::move(raise);
}

void LinkRequestCoordinator::setParkDeadlineMs(int ms)
{
    m_parkDeadlineMs = ms;
    if (m_parkDeadline.isActive())
        m_parkDeadline.start(m_parkDeadlineMs);
}

void LinkRequestCoordinator::onRegistryReady()
{
    if (m_ready)
        return;
    m_ready = true;
    m_parkDeadline.stop();
    drainInbox();
}

void LinkRequestCoordinator::drainInbox()
{
    if (!m_ready) {
        // Leave it queued; the inbox IS the cold-start stash. Start the
        // deadline so a rebuild that never arrives is reported rather than
        // leaving the click looking like it did nothing.
        if (!LinkUrlInbox::instance().isEmpty() && !m_parkDeadline.isActive())
            m_parkDeadline.start(m_parkDeadlineMs);
        return;
    }

    for (const QString& url : LinkUrlInbox::instance().takeAll())
        handleUrl(url);
}

void LinkRequestCoordinator::handleUrl(const QString& rawUrl)
{
    const LinkUrl::Request request = LinkUrl::parse(rawUrl);

    switch (request.kind) {
    case LinkUrl::Request::Invalid:
        // The URL itself is NOT echoed into the user-facing string: it is
        // attacker-supplied, and a shell dialog rendering it is exactly the
        // "must not be papered over by displaying an attacker-supplied string"
        // problem. The full text goes to the log, which is ours.
        qWarning().noquote() << "LinkRequestCoordinator: refusing" << rawUrl
                             << "—" << request.error;
        fail(QStringLiteral("A link could not be opened: %1").arg(request.error));
        return;

    case LinkUrl::Request::RaiseOnly:
        if (m_raise) m_raise();
        return;

    case LinkUrl::Request::Intent:
        break;
    }

    if (!m_broker || !m_registry) {
        fail(QStringLiteral("A link could not be opened: the intent system is "
                            "not available."));
        return;
    }

    // ONE AT A TIME. The broker queues choosers rather than repointing one
    // under the user's cursor, so without this a page could stack consent
    // dialogs by navigating repeatedly.
    if (m_inFlight) {
        qWarning().noquote()
            << "LinkRequestCoordinator: a link request is already in flight —"
               " dropping" << rawUrl;
        fail(QStringLiteral("Another link is already being handled."));
        return;
    }

    // Raise first. The user clicked a link and is looking at their browser;
    // arriving at a consent dialog on a window that never came forward is
    // worse than arriving at a window that then asks.
    if (m_raise) m_raise();

    m_inFlight = true;
    const QString requestId =
        QStringLiteral("link-%1").arg(++m_requestSerial);

    // Everything past here is the ordinary path. The broker performs the
    // `uses` check against this requester's derived web-reachable set, the
    // restriction check, resolution, consent and dispatch, with no link-shaped
    // special case anywhere in it.
    // The one success line on this path, and it is load-bearing beyond
    // debugging: a link that arrives over the single-instance socket leaves no
    // other trace in the receiving process, so without it "the second instance
    // forwarded" and "the first instance acted on it" cannot be told apart
    // from outside. The doc-test's warm-path step greps for exactly this.
    //
    // The INTENT NAME, never the raw URL: the name has passed isValidName and
    // is therefore a constrained character set, while the URL is arbitrary
    // attacker-supplied text and the failure path already logs it deliberately.
    qInfo().noquote() << "LinkRequestCoordinator: dispatching link request"
                      << requestId << "for intent" << request.intent;

    m_broker->submit(m_endpoint.get(), requestId, request.intent, request.params);
}

void LinkRequestCoordinator::onResult(const QString& requestId,
                                      const QVariantMap& envelope)
{
    m_inFlight = false;

    const bool ok = envelope.value(QStringLiteral("ok")).toBool();
    const QString error = envelope.value(QStringLiteral("error")).toString();

    if (!ok) {
        qWarning().noquote() << "LinkRequestCoordinator:" << requestId
                             << "failed —" << error;
        // Deliberately not distinguishing the six codes for the user. From a
        // link, `not_declared` means "not published to the web" and
        // `unavailable` means "nothing installed, or you were refused" — and
        // the merge of those two is a designed property, not an oversight.
        fail(QStringLiteral("The link could not be completed."));
    }

    // NOT a resume: drainInbox() emptied the queue when this request started,
    // and anything that arrived alongside it was already dropped by the
    // in-flight cap above. The call is here so a URL that lands WHILE a request
    // is being serviced is picked up as soon as it finishes, rather than
    // waiting for the next post.
    drainInbox();
}

void LinkRequestCoordinator::fail(const QString& reason)
{
    emit linkFailed(reason);
}
