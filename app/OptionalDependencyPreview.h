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

    // Optionals already installed are kept as they are, so they are not offered.
    void setExcluded(QSet<QString> names) { m_excluded = std::move(names); }

    // The subject plus every required-row pin, so a pin applies even when the
    // first pass fails; later passes keep only pins that are still mandatory.
    QString initialRequest() const
    {
        QJsonArray arr{QJsonObject::fromVariantMap(subjectRequest())};
        for (auto it = m_pins.cbegin(); it != m_pins.cend(); ++it)
            if (it.key() != m_subject && !it.value().toString().isEmpty())
                arr.append(QJsonObject{{"name", it.key()}, {"version", it.value().toString()}});
        return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    QString advance(const QVariantList& resolved)
    {
        QVariantMap graph;
        QMap<QString, QVariantMap> rows;
        // Compare this pass's artifacts per name (a plan may hold two rows of one name).
        QMap<QString, QStringList> artifacts;
        for (const QVariant& v : resolved) {
            const QVariantMap row = v.toMap();
            artifacts[row.value("name").toString()].append(
                row.value("version").toString() + "|" + row.value("rootHash").toString());
        }
        for (auto a = artifacts.begin(); a != artifacts.end(); ++a) {
            a->sort();
            const QString artifact = a->join(',');
            if (m_artifacts.contains(a.key()) && m_artifacts.value(a.key()) != artifact) {
                for (auto it = m_discovered.begin(); it != m_discovered.end();)
                    if (it.value().value("requiredBy").toString() == a.key()) it = m_discovered.erase(it);
                    else ++it;
            }
            m_artifacts.insert(a.key(), artifact);
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
                if (m_selection.value(name, true).toBool() && !m_excluded.contains(name)) queue.append(name);
            }
        }

        m_offers.clear();
        QMap<QString, int> offered;
        for (auto it = m_discovered.begin(); it != m_discovered.end();) {
            const auto offer = it.value();
            const QString name = offer.value("name").toString();
            if (!reachable.contains(offer.value("requiredBy").toString())) {
                it = m_discovered.erase(it);
                continue;
            }
            ++it;
            if (mandatory.contains(name) || m_excluded.contains(name)) continue;
            // One row per name; an available offer wins over an unavailable one.
            if (!offered.contains(name)) {
                offered.insert(name, m_offers.size());
                m_offers.append(offer);
            } else if (m_offers.at(offered.value(name)).toMap().contains("error") && !offer.contains("error")) {
                m_offers[offered.value(name)] = offer;
            }
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
        requests.insert(m_subject, subjectRequest());
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
        // The subject first: the resolver emits its closure before the optionals.
        arr.append(QJsonObject::fromVariantMap(requests.take(m_subject)));
        for (const auto& request : requests) arr.append(QJsonObject::fromVariantMap(request));
        return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    QVariantList offers() const { return m_offers; }
    QString subject() const { return m_subject; }
    QStringList optionalNames() const { return m_optionalNames; }

private:
    QVariantMap subjectRequest() const
    {
        return QJsonDocument::fromJson(gateResolverRequest(m_subject, m_repository,
            m_pins.value(m_subject).toString()).toUtf8()).array().first().toObject().toVariantMap();
    }

    QString m_subject, m_repository;
    QSet<QString> m_excluded;
    QVariantMap m_pins, m_selection, m_optionalPins;
    QMap<QString, QVariantMap> m_discovered;
    QMap<QString, QString> m_artifacts;
    QVariantList m_offers;
    QStringList m_optionalNames;
};
} // namespace logos
