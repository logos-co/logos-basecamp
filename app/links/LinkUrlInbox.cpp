#include "LinkUrlInbox.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QFileOpenEvent>

namespace {

// Private to this file: nothing else needs to name it.
class MacUrlEventFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::FileOpen) {
            auto* fileEvent = static_cast<QFileOpenEvent*>(event);
            const QUrl url = fileEvent->url();
            if (url.isValid() && !url.scheme().isEmpty())
                LinkUrlInbox::instance().post(url.toString());
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

LinkUrlInbox& LinkUrlInbox::instance()
{
    static LinkUrlInbox inbox;
    return inbox;
}

void LinkUrlInbox::post(const QString& url)
{
    if (url.isEmpty())
        return;

    if (m_queued.size() >= kMaxQueued) {
        // Loudly. A silently discarded link is indistinguishable from one that
        // was never registered, which is the hardest version of this to debug.
        qWarning().noquote()
            << "LinkUrlInbox: queue full (" << kMaxQueued << ") — dropping" << url;
        return;
    }

    m_queued.append(url);
    emit urlPosted();
}

QStringList LinkUrlInbox::takeAll()
{
    QStringList taken;
    taken.swap(m_queued);
    return taken;
}

bool LinkUrlInbox::isEmpty() const
{
    return m_queued.isEmpty();
}

void LinkUrlInbox::installEventFilter(QCoreApplication* app)
{
    if (!app) return;
    app->installEventFilter(new MacUrlEventFilter(app));
}
