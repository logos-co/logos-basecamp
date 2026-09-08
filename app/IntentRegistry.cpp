#include "IntentRegistry.h"

#include "LogosIntent.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace {

constexpr const char* kTypeUiQml = "ui_qml";

bool isReservedNamespace(const QString& name)
{
    return logos::intent::isReservedName(name)
        || name.startsWith(QStringLiteral("basecamp."));
}

// Read <installDir>/metadata.json. Returns an empty map on any failure — a
// malformed file is a diagnostic, never a crash and never a partial record.
QVariantMap readMetadataFile(const QString& installDir, QString* errorOut)
{
    const QString path = QDir(installDir).filePath(QStringLiteral("metadata.json"));

    QFile file(path);
    if (!file.exists()) {
        *errorOut = QStringLiteral("no metadata.json at %1").arg(path);
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *errorOut = QStringLiteral("cannot read %1: %2").arg(path, file.errorString());
        return {};
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *errorOut = QStringLiteral("malformed JSON in %1: %2")
                        .arg(path, parseError.errorString());
        return {};
    }
    return doc.object().toVariantMap();
}

// Parse a `provides` / `uses` array. Both are arrays of OBJECTS — a bare string
// array is refused rather than quietly accepted, because tolerating two shapes
// is how a surface stops being frozen.
QStringList parseIntentArray(const QVariant& raw,
                             const QString& moduleName,
                             const QString& keyName,
                             bool allowCardinality,
                             QStringList* diagnostics)
{
    QStringList result;
    if (!raw.isValid())
        return result;

    if (raw.typeId() != QMetaType::QVariantList) {
        diagnostics->append(QStringLiteral("%1: '%2' is not a list — ignored")
                                .arg(moduleName, keyName));
        return result;
    }

    const QVariantList entries = raw.toList();
    for (const QVariant& entry : entries) {
        if (entry.typeId() != QMetaType::QVariantMap) {
            diagnostics->append(
                QStringLiteral("%1: '%2' entry must be an object like "
                               "{\"intent\": \"a.b\"}, not a bare string — ignored")
                    .arg(moduleName, keyName));
            continue;
        }

        const QVariantMap object = entry.toMap();
        const QString intent = object.value(QStringLiteral("intent")).toString();

        if (intent.isEmpty()) {
            diagnostics->append(QStringLiteral("%1: '%2' entry has no 'intent' — ignored")
                                    .arg(moduleName, keyName));
            continue;
        }
        if (!logos::intent::isValidName(intent)) {
            diagnostics->append(QStringLiteral("%1: '%2' intent '%3' fails the name grammar — ignored")
                                    .arg(moduleName, keyName, intent));
            continue;
        }
        if (result.contains(intent)) {
            diagnostics->append(QStringLiteral("%1: '%2' declares '%3' twice — ignored")
                                    .arg(moduleName, keyName, intent));
            continue;
        }

        if (allowCardinality) {
            // Parsed and validated so a future value cannot arrive unnoticed;
            // ignored in V1, where the only behaviour is "pick one".
            const QVariant cardinality = object.value(QStringLiteral("cardinality"));
            if (cardinality.isValid()
                && cardinality.toString() != QStringLiteral("single")) {
                diagnostics->append(
                    QStringLiteral("%1: '%2' intent '%3' requests cardinality '%4', "
                                   "which is not supported — treated as 'single'")
                        .arg(moduleName, keyName, intent, cardinality.toString()));
            }
        }

        result.append(intent);
    }
    return result;
}

} // namespace

IntentRegistry::IntentRegistry(QObject* parent)
    : QObject(parent)
{
}

void IntentRegistry::reset()
{
    m_provides.clear();
    m_uses.clear();
    m_paramsSpec.clear();
    m_handoff.clear();
    m_entries.clear();
    m_diagnostics.clear();
    m_webReachable.clear();
}

void IntentRegistry::rebuild(const QMap<QString, QVariantMap>& plugins,
                             const LabelFn& labelFor,
                             const IconFn& iconFor)
{
    const QString shell = m_shellModuleName;
    const QString link = m_linkModuleName;
    const QStringList shellWeb = m_shellWebIntents;
    const QStringList shellIntents = shell.isEmpty() ? QStringList()
                                                     : m_provides.value(shell);
    const QStringList shellUses = shell.isEmpty() ? QStringList()
                                                  : m_uses.value(shell);
    const ProviderEntry shellEntry = shell.isEmpty() ? ProviderEntry()
                                                     : m_entries.value(shell);
    QStringList shellHandoff;
    for (const QString& intent : shellIntents) {
        if (m_handoff.contains(shell + QLatin1Char('/') + intent))
            shellHandoff.append(intent);
    }

    // Clear and refill. Never incremental: a half-updated index is worse than
    // a slightly stale one, and every caller re-resolves on every request.
    reset();

    // Both the shell's and the link requester's registrations are code, not
    // disk, so they survive the wipe.
    m_linkModuleName = link;
    m_shellWebIntents = shellWeb;
    if (!shell.isEmpty()) {
        m_shellModuleName = shell;
        m_provides.insert(shell, shellIntents);
        if (!shellUses.isEmpty()) m_uses.insert(shell, shellUses);
        m_entries.insert(shell, shellEntry);
        for (const QString& intent : shellHandoff)
            m_handoff.insert(shell + QLatin1Char('/') + intent);
    }

    for (auto it = plugins.cbegin(); it != plugins.cend(); ++it)
        ingestRecord(it.key(), it.value(), labelFor, iconFor);

    // The link requester's `uses` is DERIVED, never stored: the union of the
    // shell's own web-reachable intents and every disk record that opted in
    // with `"web": true`. Recomputed here, after the ingest loop, so it cannot
    // go stale against what is actually installed — and so there is no second
    // place to update when an app is added or removed.
    //
    // This is what makes the broker's existing `declaresUse` gate do the work:
    // a link naming an intent nobody published fails `not_declared` at the
    // first gate, with no bypass anywhere in the broker.
    if (!m_linkModuleName.isEmpty()) {
        QStringList reachable = m_shellWebIntents;
        for (const QString& key : m_webReachable) {
            // Keys are "<module>/<intent>"; `uses` is a set of intent NAMES.
            const int slash = key.indexOf(QLatin1Char('/'));
            if (slash < 0) continue;
            const QString intent = key.mid(slash + 1);
            if (!reachable.contains(intent))
                reachable.append(intent);
        }
        std::sort(reachable.begin(), reachable.end());
        if (!reachable.isEmpty())
            m_uses.insert(m_linkModuleName, reachable);
    }

    emit changed();
}

void IntentRegistry::ingestRecord(const QString& moduleName,
                                  const QVariantMap& metadata,
                                  const LabelFn& labelFor,
                                  const IconFn& iconFor)
{
    if (moduleName.isEmpty())
        return;

    // A disk record must never be able to claim the shell's identity, or it
    // could impersonate a shell capability by declaring one first.
    if (!m_shellModuleName.isEmpty() && moduleName == m_shellModuleName) {
        m_diagnostics.append(
            QStringLiteral("%1: an installed package may not use the shell's "
                           "module name — skipped").arg(moduleName));
        return;
    }

    // Nor the link requester's, for a sharper reason: that name's `uses` is the
    // set of web-reachable intents, so a package answering to it could add
    // itself entries and make its own capabilities reachable from a URL without
    // ever declaring `"web": true`.
    if (!m_linkModuleName.isEmpty() && moduleName == m_linkModuleName) {
        m_diagnostics.append(
            QStringLiteral("%1: an installed package may not use the link "
                           "requester's module name — skipped").arg(moduleName));
        return;
    }

    if (m_provides.contains(moduleName) || m_uses.contains(moduleName)) {
        m_diagnostics.append(
            QStringLiteral("%1: duplicate module name — first record wins").arg(moduleName));
        return;
    }

    // ui_qml only, and that is a DESIGN LINE rather than a V1 shortcut. Intents
    // exist for user-mediated actions; core modules already call each other by
    // name through LogosAPI, with no chooser and nothing to consent to. There is
    // nothing here to "fix" by widening the type check.
    const QString type = metadata.value(QStringLiteral("type")).toString();
    if (type != QLatin1String(kTypeUiQml))
        return;

    const QString installDir = metadata.value(QStringLiteral("installDir")).toString();
    if (installDir.isEmpty())
        return;

    QString error;
    const QVariantMap onDisk = readMetadataFile(installDir, &error);
    if (onDisk.isEmpty()) {
        if (!error.isEmpty())
            m_diagnostics.append(QStringLiteral("%1: %2").arg(moduleName, error));
        return;
    }

    QStringList provides = parseIntentArray(onDisk.value(QStringLiteral("provides")),
                                            moduleName, QStringLiteral("provides"),
                                            /*allowCardinality=*/false, &m_diagnostics);

    // The provider's own description of each payload, kept beside the names.
    // Read straight through: it is documentation for a caller, not something
    // the broker acts on, so it gets no grammar of its own beyond needing a
    // name per entry.
    for (const QVariant& raw : onDisk.value(QStringLiteral("provides")).toList()) {
        if (raw.typeId() != QMetaType::QVariantMap) continue;
        const QVariantMap entry = raw.toMap();
        const QString intent = entry.value(QStringLiteral("intent")).toString();
        if (intent.isEmpty() || !provides.contains(intent)) continue;

        QVariantList specs;
        for (const QVariant& p : entry.value(QStringLiteral("params")).toList()) {
            if (p.typeId() != QMetaType::QVariantMap) continue;
            const QVariantMap spec = p.toMap();
            if (spec.value(QStringLiteral("name")).toString().isEmpty()) continue;
            specs.append(spec);
        }
        if (!specs.isEmpty())
            m_paramsSpec.insert(moduleName + QLatin1Char('/') + intent, specs);

        // Refused rather than coerced. `"handoff": "true"` is a string, and
        // QVariant::toBool() would read it as true — silently giving a
        // transactional intent the opposite navigation from the one its author
        // wrote down. A diagnostic and the default is the honest answer.
        const QVariant handoff = entry.value(QStringLiteral("handoff"));
        if (handoff.isValid()) {
            if (handoff.typeId() == QMetaType::Bool) {
                if (handoff.toBool())
                    m_handoff.insert(moduleName + QLatin1Char('/') + intent);
            } else {
                m_diagnostics.append(
                    QStringLiteral("%1: '%2' declares handoff as %3, not a boolean "
                                   "— ignored, treated as false")
                        .arg(moduleName, intent,
                             QString::fromUtf8(handoff.typeName())));
            }
        }

        // OPT-IN, PER INTENT, BY THE PROVIDER. Without this every entry in
        // every app's `provides` would silently become a web entry point the
        // moment link support shipped. `uses` cannot gate a link — there is no
        // manifest on the calling side — so reachability has to be declared by
        // the side that will service it.
        //
        // Strict bool for the same reason as handoff, and it matters more here:
        // `"web": "false"` read as true would publish a capability its author
        // was explicitly declining to publish.
        const QVariant web = entry.value(QStringLiteral("web"));
        if (web.isValid()) {
            if (web.typeId() == QMetaType::Bool) {
                if (web.toBool())
                    m_webReachable.insert(moduleName + QLatin1Char('/') + intent);
            } else {
                m_diagnostics.append(
                    QStringLiteral("%1: '%2' declares web as %3, not a boolean "
                                   "— ignored, treated as false")
                        .arg(moduleName, intent,
                             QString::fromUtf8(web.typeName())));
            }
        }
    }
    const QStringList uses = parseIntentArray(onDisk.value(QStringLiteral("uses")),
                                              moduleName, QStringLiteral("uses"),
                                              /*allowCardinality=*/true, &m_diagnostics);

    // "logos.*" is the platform's and "basecamp.*" is the shell's. Refuse both
    // from anything else, so an app cannot register a shell capability and
    // intercept requests meant for it.
    for (int i = provides.size() - 1; i >= 0; --i) {
        if (isReservedNamespace(provides.at(i))) {
            m_diagnostics.append(
                QStringLiteral("%1: refused to provide reserved intent '%2' — "
                               "the 'logos.' and 'basecamp.' namespaces are "
                               "reserved for the platform and the shell")
                    .arg(moduleName, provides.at(i)));
            provides.removeAt(i);
        }
    }

    if (provides.isEmpty() && uses.isEmpty())
        return;

    if (!provides.isEmpty()) m_provides.insert(moduleName, provides);
    if (!uses.isEmpty())     m_uses.insert(moduleName, uses);

    ProviderEntry entry;
    entry.moduleName = moduleName;
    entry.displayName = labelFor ? labelFor(moduleName) : moduleName;
    entry.iconSource = iconFor ? iconFor(moduleName) : QString();
    if (entry.displayName.isEmpty()) entry.displayName = moduleName;
    m_entries.insert(moduleName, entry);
}

void IntentRegistry::setInstallableProviders(
    const QMap<QString, QStringList>& byModuleName)
{
    // Clear-and-refill, like rebuild(). A half-updated index is worse than a
    // slightly late one.
    m_installable.clear();

    for (auto it = byModuleName.cbegin(); it != byModuleName.cend(); ++it) {
        const QString& moduleName = it.key();

        // An INSTALLED package is answered by the real table; listing it here
        // too would let the shell offer to install something already present.
        if (m_entries.contains(moduleName))
            continue;

        for (const QString& intent : it.value()) {
            // Same grammar and same reservation as an on-disk declaration. A
            // catalog is a less trusted source than the local disk, not a more
            // trusted one, so it does not get a laxer filter.
            if (!logos::intent::isValidName(intent))
                continue;
            if (logos::intent::isReservedName(intent)) {
                m_diagnostics.append(
                    QStringLiteral("catalog: %1 offers reserved intent '%2' — ignored")
                        .arg(moduleName, intent));
                continue;
            }
            QStringList& names = m_installable[intent];
            if (!names.contains(moduleName))
                names.append(moduleName);
        }
    }

    for (auto it = m_installable.begin(); it != m_installable.end(); ++it)
        it.value().sort();

    emit changed();
}

QStringList IntentRegistry::installableProvidersFor(const QString& intent) const
{
    return m_installable.value(intent);
}

QVariantList IntentRegistry::paramsSpecFor(const QString& moduleName,
                                           const QString& intent) const
{
    return m_paramsSpec.value(moduleName + QLatin1Char('/') + intent);
}

bool IntentRegistry::isHandoff(const QString& moduleName,
                               const QString& intent) const
{
    return m_handoff.contains(moduleName + QLatin1Char('/') + intent);
}

bool IntentRegistry::isShellProvider(const QString& moduleName) const
{
    return !m_shellModuleName.isEmpty() && moduleName == m_shellModuleName;
}

void IntentRegistry::registerShellUses(const QString& shellModuleName,
                                       const QStringList& intents)
{
    if (shellModuleName.isEmpty())
        return;

    m_shellModuleName = shellModuleName;

    QStringList accepted;
    for (const QString& intent : intents) {
        if (!logos::intent::isValidName(intent)) {
            m_diagnostics.append(
                QStringLiteral("shell: uses '%1' fails the name grammar — ignored")
                    .arg(intent));
            continue;
        }
        accepted.append(intent);
    }
    // Survives rebuild() the same way the shell's provides does — rebuild()
    // preserves m_shellModuleName, and the shell is not a disk record.
    m_uses.insert(shellModuleName, accepted);

    emit changed();
}

void IntentRegistry::registerShellProvider(const QString& shellModuleName,
                                           const QStringList& intents,
                                           const QStringList& handoffIntents,
                                           const QString& displayName,
                                           const QString& iconSource)
{
    if (shellModuleName.isEmpty())
        return;

    m_shellModuleName = shellModuleName;

    QStringList accepted;
    for (const QString& intent : intents) {
        if (!logos::intent::isValidName(intent)) {
            m_diagnostics.append(
                QStringLiteral("shell: intent '%1' fails the name grammar — ignored").arg(intent));
            continue;
        }
        accepted.append(intent);
    }

    // Only over intents that survived the grammar check: a hand-off flag on a
    // name the registry refused would sit in the table describing nothing.
    for (const QString& intent : handoffIntents) {
        if (accepted.contains(intent))
            m_handoff.insert(shellModuleName + QLatin1Char('/') + intent);
        else
            m_diagnostics.append(
                QStringLiteral("shell: handoff declared for '%1', which is not a "
                               "registered shell provider intent — ignored").arg(intent));
    }

    m_provides.insert(shellModuleName, accepted);

    ProviderEntry entry;
    entry.moduleName = shellModuleName;
    entry.displayName = displayName.isEmpty() ? shellModuleName : displayName;
    entry.iconSource = iconSource;
    m_entries.insert(shellModuleName, entry);

    emit changed();
}

void IntentRegistry::restrictIntentToRequesters(const QString& intent,
                                                const QStringList& requesters)
{
    if (intent.isEmpty())
        return;

    // An empty list would read as "restricted to nobody" but store as
    // "unrestricted" — refuse it rather than silently opening the intent up.
    if (requesters.isEmpty()) {
        m_diagnostics.append(
            QStringLiteral("shell: refusing empty requester list for '%1'").arg(intent));
        return;
    }

    m_restrictedIntents.insert(intent, requesters);
}

bool IntentRegistry::requesterAllowed(const QString& intent,
                                      const QString& requesterName) const
{
    const auto it = m_restrictedIntents.constFind(intent);
    if (it == m_restrictedIntents.cend())
        return true;
    return it.value().contains(requesterName);
}

IntentRegistry::Resolution IntentRegistry::resolve(const QString& intent) const
{
    Resolution resolution;
    if (intent.isEmpty())
        return resolution;

    for (auto it = m_provides.cbegin(); it != m_provides.cend(); ++it) {
        if (!it.value().contains(intent))
            continue;
        resolution.found.append(m_entries.value(it.key(),
                                                ProviderEntry{ it.key(), it.key(), QString() }));
    }

    // Sorted by module name, not by map order or install order. A chooser whose
    // rows move between runs is both confusing and untestable.
    std::sort(resolution.found.begin(), resolution.found.end(),
              [](const ProviderEntry& a, const ProviderEntry& b) {
                  return a.moduleName < b.moduleName;
              });

    if (resolution.found.isEmpty())      resolution.status = None;
    else if (resolution.found.size() == 1) resolution.status = Ok;
    else                                   resolution.status = Ambiguous;

    return resolution;
}

bool IntentRegistry::declaresUse(const QString& moduleName, const QString& intent) const
{
    return m_uses.value(moduleName).contains(intent);
}

bool IntentRegistry::declaresProvide(const QString& moduleName, const QString& intent) const
{
    return m_provides.value(moduleName).contains(intent);
}

QStringList IntentRegistry::diagnostics() const
{
    return m_diagnostics;
}

void IntentRegistry::registerLinkRequester(const QString& linkModuleName,
                                           const QStringList& shellWebIntents)
{
    if (linkModuleName.isEmpty())
        return;

    // A link is neither the shell nor an app, and needs its own identity for
    // both halves of that. Not the shell: isShellProvider() is what makes the
    // broker skip the chooser, and a URL must never inherit that. Not an app:
    // there is no metadata.json on the calling side, so its `uses` has to come
    // from somewhere else — see the derivation in rebuild().
    m_linkModuleName = linkModuleName;

    QStringList accepted;
    for (const QString& intent : shellWebIntents) {
        if (!logos::intent::isValidName(intent)) {
            m_diagnostics.append(
                QStringLiteral("link: shell web intent '%1' fails the name "
                               "grammar — ignored").arg(intent));
            continue;
        }
        accepted.append(intent);
    }
    m_shellWebIntents = accepted;

    emit changed();
}

bool IntentRegistry::isLinkRequester(const QString& moduleName) const
{
    return !m_linkModuleName.isEmpty() && moduleName == m_linkModuleName;
}

bool IntentRegistry::isWebReachable(const QString& moduleName,
                                    const QString& intent) const
{
    // PER PROVIDER, like handoff, and for a sharper reason. Keyed on the intent
    // name alone, one app declaring `"web": true` would publish EVERY
    // provider's implementation of that name — including apps that deliberately
    // left the flag off. The opt-in is the author's statement about their own
    // app, so it has to be recorded against their own app.
    if (isShellProvider(moduleName))
        return m_shellWebIntents.contains(intent);
    return m_webReachable.contains(moduleName + QLatin1Char('/') + intent);
}

IntentRegistry::Resolution IntentRegistry::resolveFor(const QString& requesterName,
                                                      const QString& intent) const
{
    Resolution resolution = resolve(intent);
    if (!isLinkRequester(requesterName))
        return resolution;

    // A link may only ever reach a provider that opted in. The `uses` gate
    // upstream is a union across every installed app, so on its own it decides
    // only that SOMETHING published this name — not that the app about to be
    // offered did.
    //
    // Filtered here rather than in the broker so the broker learns nothing
    // about links: it passes the requester name it already holds, and which
    // requesters are constrained stays policy, in the disposable half.
    for (int i = resolution.found.size() - 1; i >= 0; --i) {
        if (!isWebReachable(resolution.found.at(i).moduleName, intent))
            resolution.found.removeAt(i);
    }
    resolution.status = resolution.found.isEmpty() ? None
                      : (resolution.found.size() == 1 ? Ok : Ambiguous);
    return resolution;
}
