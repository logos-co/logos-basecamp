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
    m_entries.clear();
    m_diagnostics.clear();
}

void IntentRegistry::rebuild(const QMap<QString, QVariantMap>& plugins,
                             const LabelFn& labelFor,
                             const IconFn& iconFor)
{
    const QString shell = m_shellModuleName;
    const QStringList shellIntents = shell.isEmpty() ? QStringList()
                                                     : m_provides.value(shell);
    const QStringList shellUses = shell.isEmpty() ? QStringList()
                                                  : m_uses.value(shell);
    const ProviderEntry shellEntry = shell.isEmpty() ? ProviderEntry()
                                                     : m_entries.value(shell);

    // Clear and refill. Never incremental: a half-updated index is worse than
    // a slightly stale one, and every caller re-resolves on every request.
    reset();

    // The shell's registration is code, not disk, so it survives the wipe.
    if (!shell.isEmpty()) {
        m_shellModuleName = shell;
        m_provides.insert(shell, shellIntents);
        if (!shellUses.isEmpty()) m_uses.insert(shell, shellUses);
        m_entries.insert(shell, shellEntry);
    }

    for (auto it = plugins.cbegin(); it != plugins.cend(); ++it)
        ingestRecord(it.key(), it.value(), labelFor, iconFor);

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
    }
    const QStringList uses = parseIntentArray(onDisk.value(QStringLiteral("uses")),
                                              moduleName, QStringLiteral("uses"),
                                              /*allowCardinality=*/true, &m_diagnostics);

    // "logos.*" belongs to the shell. Refuse it from anything else, so an app
    // cannot register a shell capability and intercept requests meant for it.
    for (int i = provides.size() - 1; i >= 0; --i) {
        if (logos::intent::isReservedName(provides.at(i))) {
            m_diagnostics.append(
                QStringLiteral("%1: refused to provide reserved intent '%2' — "
                               "the 'logos.' namespace belongs to the shell")
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
