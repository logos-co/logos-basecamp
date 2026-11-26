#include "ConfirmationQueue.h"

#include <QDebug>
#include <QHash>
#include <QTimer>

namespace {

const QString kConfirmUninstall = QStringLiteral("basecamp.packages.confirm_uninstall");

}  // namespace

ConfirmationQueue::ConfirmationQueue(QObject* parent)
    : QObject(parent)
    , m_changeTimer(new QTimer(this))
{
    m_changeTimer->setSingleShot(true);
    connect(m_changeTimer, &QTimer::timeout, this, [this]() {
        qWarning().noquote() << "ConfirmationQueue: nothing of the change" << m_change.requester
                             << "was approved for is on its way; showing the next anyway";
        endChange();
        next();
    });
}

void ConfirmationQueue::submit(const Request& request)
{
    // An installer app asks again only once its last change has ended.
    if (!m_change.requester.isEmpty() && m_change.requester == request.requester) endChange();
    m_waiting.append(request);
    next();
}

void ConfirmationQueue::answered(const QString& id, bool approved, const QVariant& data)
{
    if (id.isEmpty() || id != m_onScreen.id) return;
    const Request request = m_onScreen;
    m_onScreen = {};
    if (approved) {
        Change change{request.requester, {}, {}};
        const QVariantMap answer = data.toMap();
        if (request.intent == kConfirmUninstall) {
            // What the answer names: an app goes with what only it needed.
            change.removes = answer.contains(QStringLiteral("names"))
                ? answer.value(QStringLiteral("names")).toStringList()
                : request.params.value(QStringLiteral("names")).toStringList();
        } else {
            for (const QVariant& v : answer.value(QStringLiteral("plan")).toList()) {
                const QVariantMap row = v.toMap();
                change.installs.insert(row.value(QStringLiteral("name")).toString(),
                                       row.value(QStringLiteral("version")).toString());
            }
            // A local file: the package it names.
            if (change.installs.isEmpty())
                change.installs.insert(request.params.value(QStringLiteral("name")).toString(),
                                       request.params.value(QStringLiteral("version")).toString());
        }
        change.installs.remove(QString());
        if (!change.installs.isEmpty() || !change.removes.isEmpty()) {
            m_change = change;
            m_changeTimer->start(m_changeTimeoutMs);
        }
    }
    // Answered from inside begin(): submit() goes on from there.
    if (!m_beginning) next();
}

void ConfirmationQueue::withdrawn(const QString& id)
{
    if (id.isEmpty()) return;
    if (id == m_onScreen.id) {
        m_onScreen = {};
        if (!m_beginning) next();
        return;
    }
    for (int i = 0; i < m_waiting.size(); ++i) {
        if (m_waiting.at(i).id == id) {
            m_waiting.removeAt(i);
            return;
        }
    }
}

void ConfirmationQueue::installedChanged(const QVariantList& installed)
{
    if (m_change.requester.isEmpty() || !madeOn(installed)) return;
    endChange();
    next();
}

void ConfirmationQueue::progressed(const QString& name)
{
    if (m_change.requester.isEmpty() || !m_change.installs.contains(name)) return;
    m_changeTimer->start(m_changeTimeoutMs);
}

void ConfirmationQueue::requesterStopped(const QString& requester)
{
    if (requester.isEmpty() || requester != m_change.requester) return;
    endChange();
    next();
}

bool ConfirmationQueue::madeOn(const QVariantList& installed) const
{
    QHash<QString, QString> versions;
    for (const QVariant& v : installed) {
        const QVariantMap package = v.toMap();
        versions.insert(package.value(QStringLiteral("name")).toString(),
                        package.value(QStringLiteral("version")).toString());
    }
    for (auto it = m_change.installs.cbegin(); it != m_change.installs.cend(); ++it) {
        const auto found = versions.constFind(it.key());
        if (found == versions.cend()) return false;
        const QString version = it.value().toString();
        if (!version.isEmpty() && found.value() != version) return false;
    }
    for (const QString& name : m_change.removes) {
        if (versions.contains(name)) return false;
    }
    return true;
}

void ConfirmationQueue::endChange()
{
    m_change = {};
    m_changeTimer->stop();
}

void ConfirmationQueue::next()
{
    while (m_onScreen.id.isEmpty() && m_change.requester.isEmpty() && !m_waiting.isEmpty()) {
        m_onScreen = m_waiting.takeFirst();
        const Request request = m_onScreen;
        m_beginning = true;
        const bool shown = m_begin && m_begin(request);
        m_beginning = false;
        // Not shown, or answered at once: the next one, if nothing else holds it.
        if (!shown && m_onScreen.id == request.id) m_onScreen = {};
    }
}
