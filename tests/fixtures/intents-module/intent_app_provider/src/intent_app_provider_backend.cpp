#include "intent_app_provider_backend.h"

#include "logos_sdk.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

QString IntentAppProviderBackend::deliver(const QString& dispatch, const QString& intent,
                                          const QString& paramsJson, const QString& requester)
{
    // A hand-off, answered on arrival: here only to show who asked for what.
    if (intent == QStringLiteral("packages.show")) {
        const QString shown = requester + QStringLiteral(" shows: ")
            + QJsonDocument::fromJson(paramsJson.toUtf8()).object().value(QStringLiteral("name")).toString();
        m_dispatch = dispatch;
        QTimer::singleShot(0, this, [this, shown]() {
            respond(true, QStringLiteral("{}"), QString());
            setRequest(shown);
        });
        return QStringLiteral("{\"ok\":true}");
    }
    if (intent != QStringLiteral("test.app_echo")) return QStringLiteral("{\"ok\":false}");
    m_dispatch = dispatch;
    m_requester = requester;
    m_text = QJsonDocument::fromJson(paramsJson.toUtf8()).object().value(QStringLiteral("text")).toString();
    setRequest(requester + QStringLiteral(" asks: ") + m_text);
    return QStringLiteral("{\"ok\":true}");
}

void IntentAppProviderBackend::approve()
{
    const QJsonObject data{{QStringLiteral("echo"), m_text}, {QStringLiteral("requester"), m_requester}};
    respond(true, QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)), QString());
}

void IntentAppProviderBackend::decline()
{
    respond(false, QString(), QStringLiteral("cancelled"));
}

void IntentAppProviderBackend::respond(bool ok, const QString& dataJson, const QString& error)
{
    if (m_dispatch.isEmpty() || !isContextReady()) return;
    modules().bind_intents(QStringLiteral("intents_module"))
        .respondAsync(m_dispatch, ok, dataJson, error, [](QString) {}, Timeout(10000));
    m_dispatch.clear();
    setRequest(QString());
}
