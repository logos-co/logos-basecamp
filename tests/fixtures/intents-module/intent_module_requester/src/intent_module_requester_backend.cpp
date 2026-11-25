#include "intent_module_requester_backend.h"

#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_sdk.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace {

const QString kIntents = QStringLiteral("intents_module");
const QString kAsking = QStringLiteral("asking");
const QString kWaiting = QStringLiteral("waiting");
constexpr int kHistoryLines = 10;

QJsonObject parse(const QString& v)
{
    return QJsonDocument::fromJson(v.toUtf8()).object();
}

} // namespace

IntentModuleRequesterBackend::Request* IntentModuleRequesterBackend::find(int number)
{
    for (Request& r : m_requests)
        if (r.number == number) return &r;
    return nullptr;
}

void IntentModuleRequesterBackend::onContextReady()
{
    modules().bind_intents(kIntents).onSettled([this](const QString& handle) { fetchResult(handle); });
}

void IntentModuleRequesterBackend::ask(QString intent, QString text, bool clicked)
{
    const int number = ++m_next;
    m_requests.append(Request{number, clicked, {}, {}, kAsking});
    publish();

    if (!isContextReady()) {
        setOutcome(number, QStringLiteral("no intents_module"));
        return;
    }
    QJsonObject params;
    if (!text.isEmpty()) params.insert(QStringLiteral("text"), text);
    modules().bind_intents(kIntents).requestAsyncResult(intent,
        QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact)),
        [this, number](logos::AsyncResult<QString> reply) {
            const QJsonObject r = parse(reply.value);
            Request* q = find(number);
            if (!q) return;
            if (!reply.ok() || !r.value(QStringLiteral("ok")).toBool()) {
                const QString why = r.value(QStringLiteral("error")).toString();
                setOutcome(number, why.isEmpty() ? QStringLiteral("call failed")
                                                 : QStringLiteral("refused: ") + why);
                return;
            }
            q->handle = r.value(QStringLiteral("handle")).toString();
            q->receipt = r.value(QStringLiteral("receipt")).toString();
            q->outcome = kWaiting;
            publish();
            fetchResult(q->handle);   // it may have settled before this reply
        },
        Timeout(10000));
}

void IntentModuleRequesterBackend::cancelWaiting()
{
    if (!isContextReady()) return;
    // Each one settles as `cancelled`, which the `settled` event then reports.
    for (const Request& r : m_requests) {
        if (r.outcome != kWaiting) continue;
        modules().bind_intents(kIntents).cancelAsync(r.handle, r.receipt, [](QString) {}, Timeout(10000));
    }
}

void IntentModuleRequesterBackend::deliverDirectly(QString module, QString intent)
{
    LogosAPIClient* c = isContextReady() ? modules().api->getClient(module) : nullptr;
    if (!c) {
        setDirect(module + QStringLiteral(": no client"));
        return;
    }
    c->invokeRemoteMethodAsync(module, QStringLiteral("deliver"),
        QVariantList{QStringLiteral("forged-dispatch"), intent, QStringLiteral("{\"text\":\"forged\"}"),
                     QStringLiteral("intent_module_requester")},
        LogosAPIClient::AsyncResultErrorCallback([this, module](QVariant reply, const logos::CallError& err) {
            // "refused" only from the provider itself; a call that never got there is "unreachable".
            const QString outcome = !err.ok() ? QStringLiteral("unreachable")
                : parse(reply.toString()).value(QStringLiteral("ok")).toBool() ? QStringLiteral("accepted")
                : QStringLiteral("refused");
            setDirect(module + QStringLiteral(": ") + outcome);
        }),
        Timeout(10000));
}

void IntentModuleRequesterBackend::fetchResult(const QString& handle)
{
    if (!isContextReady() || handle.isEmpty()) return;
    int number = 0;
    QString receipt;
    for (const Request& r : m_requests) {
        if (r.handle == handle && r.outcome == kWaiting) {
            number = r.number;
            receipt = r.receipt;
        }
    }
    if (!number) return;
    modules().bind_intents(kIntents).resultAsyncResult(handle, receipt,
        [this, number](logos::AsyncResult<QString> reply) {
            const QJsonObject r = parse(reply.value);
            if (r.value(QStringLiteral("state")).toString() != QStringLiteral("settled")) return;
            const QJsonObject result = r.value(QStringLiteral("result")).toObject();
            if (result.value(QStringLiteral("ok")).toBool()) {
                const QJsonObject data = result.value(QStringLiteral("data")).toObject();
                setOutcome(number, data.contains(QStringLiteral("echo"))
                    ? QStringLiteral("echo: ") + data.value(QStringLiteral("echo")).toString()
                          + QStringLiteral(" (for ") + data.value(QStringLiteral("requester")).toString()
                          + QStringLiteral(")")
                    : QStringLiteral("ok"));
            } else {
                setOutcome(number, QStringLiteral("error: ") + result.value(QStringLiteral("error")).toString());
            }
        },
        Timeout(10000));
}

void IntentModuleRequesterBackend::setOutcome(int number, const QString& outcome)
{
    if (Request* r = find(number)) r->outcome = outcome;
    publish();
}

void IntentModuleRequesterBackend::publish()
{
    QStringList lines;
    int waiting = 0;
    for (auto it = m_requests.crbegin(); it != m_requests.crend(); ++it) {
        if (it->outcome == kAsking || it->outcome == kWaiting) ++waiting;
        if (lines.size() < kHistoryLines) {
            lines << QStringLiteral("#%1 %2: %3")
                         .arg(QString::number(it->number),
                              it->clicked ? QStringLiteral("clicked") : QStringLiteral("no click"),
                              it->outcome);
        }
    }
    setStatus(lines.value(0, QStringLiteral("idle")));
    setHistory(lines.join(QLatin1Char('\n')));
    setWaiting(waiting);
}
