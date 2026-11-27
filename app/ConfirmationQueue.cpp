#include "ConfirmationQueue.h"

#include <QDebug>
#include <QHash>
#include <QTimer>

namespace {

const QString kConfirmInstall = QStringLiteral("basecamp.packages.confirm_install");
const QString kConfirmUpgrade = QStringLiteral("basecamp.packages.confirm_upgrade");
const QString kConfirmUninstall = QStringLiteral("basecamp.packages.confirm_uninstall");

// Shown while another change is made, as its app holds what is approved
// until its turn.
bool canWait(const ConfirmationQueue::Request& request)
{
    return (request.intent == kConfirmInstall || request.intent == kConfirmUpgrade)
        && request.params.value(QStringLiteral("waits")).toBool()
        && !request.params.contains(QStringLiteral("ticket"));
}

}  // namespace

ConfirmationQueue::ConfirmationQueue(QObject* parent)
    : QObject(parent)
    , m_changeTimer(new QTimer(this))
{
    m_changeTimer->setSingleShot(true);
    connect(m_changeTimer, &QTimer::timeout, this, [this]() {
        qWarning().noquote() << "ConfirmationQueue: nothing of the change" << m_change.requester
                             << "was approved for is moving; showing the next anyway";
        endChange();
        next();
    });
}

void ConfirmationQueue::submit(const Request& request)
{
    // An installer app asks again only once its last change has ended, but
    // for a request that can wait.
    if (!m_change.requester.isEmpty() && m_change.requester == request.requester && !canWait(request))
        endChange();
    m_waiting.append(request);
    next();
}

void ConfirmationQueue::answered(const QString& id, bool approved, const QVariant& data)
{
    if (id.isEmpty() || id != m_onScreen.id) return;
    const Request request = m_onScreen;
    m_onScreen = {};
    // Approved for later: nothing changes until its ticket is handed back.
    if (approved && !data.toMap().contains(QStringLiteral("ticket"))) {
        Change change{request.id, request.requester, {}, {}};
        const QVariantMap answer = data.toMap();
        if (request.intent == kConfirmUninstall) {
            // What the answer names: an app goes with what only it needed.
            change.removes = answer.contains(QStringLiteral("names"))
                ? answer.value(QStringLiteral("names")).toStringList()
                : request.params.value(QStringLiteral("names")).toStringList();
        } else {
            // Optional packages too: one still installing is part of the
            // change. One that fails is left out, and its app says the change
            // ended.
            for (const QVariant& v : answer.value(QStringLiteral("plan")).toList()) {
                const QVariantMap row = v.toMap();
                change.installs.insert(row.value(QStringLiteral("name")).toString(),
                                       {row.value(QStringLiteral("version")).toString(),
                                        row.value(QStringLiteral("rootHash")).toString()});
            }
            // A local file: the package it names, as approved.
            if (change.installs.isEmpty())
                change.installs.insert(request.params.value(QStringLiteral("name")).toString(),
                                       {request.params.value(QStringLiteral("version")).toString(),
                                        answer.value(QStringLiteral("file")).toMap()
                                            .value(QStringLiteral("rootHash")).toString()});
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
    if (m_change.requester.isEmpty()
        || !(m_change.installs.contains(name) || m_change.removes.contains(name)))
        return;
    m_changeTimer->start(m_changeTimeoutMs);
}

void ConfirmationQueue::activity()
{
    if (!m_change.requester.isEmpty()) m_changeTimer->start(m_changeTimeoutMs);
}

void ConfirmationQueue::changeEnded(const QString& change)
{
    if (change.isEmpty() || change != m_change.id) return;
    endChange();
    next();
}

void ConfirmationQueue::requesterGone(const QString& requester)
{
    if (requester.isEmpty() || requester != m_change.requester) return;
    endChange();
    next();
}

bool ConfirmationQueue::madeOn(const QVariantList& installed) const
{
    QHash<QString, Release> onDisk;
    for (const QVariant& v : installed) {
        const QVariantMap package = v.toMap();
        onDisk.insert(package.value(QStringLiteral("name")).toString(),
                      {package.value(QStringLiteral("version")).toString(),
                       package.value(QStringLiteral("hashes")).toMap().value(QStringLiteral("root")).toString()});
    }
    for (auto it = m_change.installs.cbegin(); it != m_change.installs.cend(); ++it) {
        const auto found = onDisk.constFind(it.key());
        if (found == onDisk.cend()) return false;
        const Release& want = it.value();
        if (!want.version.isEmpty() && found->version != want.version) return false;
        // A release can come again under its version: a reinstall is made
        // once its own contents are there.
        if (!want.rootHash.isEmpty() && found->rootHash != want.rootHash) return false;
    }
    for (const QString& name : m_change.removes) {
        if (onDisk.contains(name)) return false;
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
    while (m_onScreen.id.isEmpty()) {
        // The first that may be shown: any once nothing is being made, one
        // that can wait whatever is.
        int at = -1;
        for (int i = 0; i < m_waiting.size() && at < 0; ++i)
            if (m_change.requester.isEmpty() || canWait(m_waiting.at(i))) at = i;
        if (at < 0) return;
        m_onScreen = m_waiting.takeAt(at);
        const Request request = m_onScreen;
        m_beginning = true;
        const bool shown = m_begin && m_begin(request);
        m_beginning = false;
        // Not shown, or answered at once: the next one, if nothing else holds it.
        if (!shown && m_onScreen.id == request.id) m_onScreen = {};
    }
}
