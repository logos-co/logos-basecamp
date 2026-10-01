#pragma once

#include "ResolverRequest.h"
#include <QMap>
#include <QSet>

namespace logos {

// Rebuilt from the subject for every UI edit. Each pass discovers offers on
// the selected graph; subsequent passes resolve their required closures.
class OptionalDependencyPreview {
public:
    OptionalDependencyPreview(QString subject, QString repository, QVariantMap pins = {},
                              QVariantMap selection = {}, QVariantMap optionalPins = {})
        : m_subject(std::move(subject)), m_repository(std::move(repository)),
          m_pins(std::move(pins)), m_selection(std::move(selection)),
          m_optionalPins(std::move(optionalPins)) {}

    QString initialRequest() const
    { return gateResolverRequest(m_subject, m_repository, m_pins.value(m_subject).toString()); }

    QString advance(const QVariantList& resolved)
    {
        QVariantMap graph;
        QMap<QString, QVariantMap> rows;
        for (const QVariant& v : resolved) {
            const QVariantMap row = v.toMap();
            const QString name = row.value("name").toString();
            const QString artifact = row.value("version").toString() + "|" + row.value("rootHash").toString();
            if (m_artifacts.contains(name) && m_artifacts.value(name) != artifact) {
                for (auto it = m_discovered.begin(); it != m_discovered.end();)
                    if (it.value().value("requiredBy").toString() == name) it = m_discovered.erase(it);
                    else ++it;
            }
            m_artifacts.insert(name, artifact);
        }
        for (const QVariant& v : resolved) {
            const QVariantMap row = v.toMap();
            rows.insert(row.value("name").toString(), row);
            const auto edges = row.value("dependencyGraph").toMap();
            for (auto it = edges.cbegin(); it != edges.cend(); ++it) graph.insert(it.key(), it.value());
            for (const QVariant& o : row.value("optionalDependencies").toList()) {
                const QVariantMap offer = o.toMap();
                m_discovered.insert(offer.value("requiredBy").toString() + QChar(31)
                                    + offer.value("name").toString(), offer);
            }
        }

        QSet<QString> reachable, mandatory;
        QStringList queue{m_subject};
        mandatory.insert(m_subject);
        for (int i = 0; i < queue.size(); ++i) {
            const QString parent = queue[i];
            if (reachable.contains(parent)) continue;
            reachable.insert(parent);
            for (const QVariant& dep : graph.value(parent).toList()) {
                const QString name = dep.typeId() == QMetaType::QString
                    ? dep.toString() : dep.toMap().value("name").toString();
                if (name.isEmpty()) continue;
                mandatory.insert(name);
                queue.append(name);
            }
            for (const QVariantMap& offer : m_discovered) {
                if (offer.value("requiredBy").toString() != parent || offer.contains("error")) continue;
                const QString name = offer.value("name").toString();
                if (m_selection.value(name, true).toBool()) queue.append(name);
            }
        }

        m_offers.clear();
        QSet<QString> offered;
        for (auto it = m_discovered.begin(); it != m_discovered.end();) {
            const auto offer = it.value();
            const QString name = offer.value("name").toString();
            if (!reachable.contains(offer.value("requiredBy").toString())) {
                it = m_discovered.erase(it);
                continue;
            }
            if (!mandatory.contains(name) && !offered.contains(name)) {
                m_offers.append(offer);
                offered.insert(name);
            }
            ++it;
        }

        m_optionalNames.clear();
        for (const QVariant& v : m_offers) {
            const QVariantMap offer = v.toMap();
            const QString name = offer.value("name").toString();
            if (!offer.contains("error") && m_selection.value(name, true).toBool())
                m_optionalNames.append(name);
        }
        QVariantList optionalRequests;
        for (const QVariant& v : m_offers) {
            const QVariantMap offer = v.toMap();
            const QString name = offer.value("name").toString();
            if (!m_optionalNames.contains(name)) continue;
            QVariantList picked = selectedOptionalRequests({offer}, {name}, m_optionalPins);
            if (picked.isEmpty()) {
                // A required-version edit may invalidate an earlier optional
                // choice. Keep the checked package at the new resolved default,
                // matching the QML picker's pruning of that stale choice.
                m_optionalPins.remove(name);
                picked = selectedOptionalRequests({offer}, {name});
            }
            optionalRequests.append(picked);
        }
        QMap<QString, QVariantMap> requests;
        requests.insert(m_subject, QJsonDocument::fromJson(initialRequest().toUtf8()).array().first().toObject().toVariantMap());
        for (const QVariant& v : optionalRequests) {
            const QVariantMap request = v.toMap();
            requests.insert(request.value("name").toString(), request);
        }
        // Only pin mandatory rows still reachable. A picker for a dependency
        // of an unchecked optional must not turn that dependency into a root.
        for (auto it = m_pins.cbegin(); it != m_pins.cend(); ++it) {
            if (it.key() == m_subject || !mandatory.contains(it.key()) || it.value().toString().isEmpty()) continue;
            QVariantMap request{{"name", it.key()}, {"version", it.value()}};
            const QString repo = rows.value(it.key()).value("repositoryUrl").toString();
            if (!repo.isEmpty()) request.insert("repositoryUrl", repo);
            requests.insert(it.key(), request);
        }
        QJsonArray arr;
        // The subject first keeps the resolver's top-level metadata stable.
        arr.append(QJsonObject::fromVariantMap(requests.take(m_subject)));
        for (const auto& request : requests) arr.append(QJsonObject::fromVariantMap(request));
        return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    QVariantList offers() const { return m_offers; }
    QString subject() const { return m_subject; }
    QStringList optionalNames() const { return m_optionalNames; }

private:
    QString m_subject, m_repository;
    QVariantMap m_pins, m_selection, m_optionalPins;
    QMap<QString, QVariantMap> m_discovered;
    QMap<QString, QString> m_artifacts;
    QVariantList m_offers;
    QStringList m_optionalNames;
};
} // namespace logos
