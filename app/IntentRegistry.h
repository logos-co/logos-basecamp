#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

// ── IntentRegistry ───────────────────────────────────────────────────────────
//
// The index of "which installed app can do what", built from each app's own
// metadata.json on disk.
//
// HARD CONSTRAINT: no logos_api.h, no UIPluginManager.h, no Widgets header.
// The unit-test harness links only Qt Core/Qml/Widgets/Quick/QuickWidgets/Test,
// and anything dragging in logos_api.h becomes untestable there — which is why
// UIPluginManager has no unit coverage. Staying dependency-free is what lets the
// resolution rules, the part with security consequences, be tested at all.
//
// WHY DISK, NOT IPC. `provides` / `uses` never reach basecamp's in-memory plugin
// cache: package_manager projects InstalledPackage through a fixed key map and
// manifest.json is regenerated from a closed struct, so unknown keys are dropped
// at both boundaries. They survive verbatim only into <installDir>/metadata.json,
// which the module builder copies byte-for-byte and the package manager installs
// whole. standalone-app already prefers that file for the same reason.
//
// Rebuilt wholesale, never incrementally, mirroring the UI-plugin cache: a stale
// half-updated index is worse than a slightly late one.
class IntentRegistry : public QObject {
    Q_OBJECT
public:
    // What resolve() found. Never a bare provider name: a status map is what
    // lets a chooser be added later without touching a single caller.
    enum Status {
        Ok,          // exactly one provider
        None,        // nobody provides it
        Ambiguous    // two or more — the caller decides how to choose
    };
    Q_ENUM(Status)

    struct ProviderEntry {
        QString moduleName;    // the identity. Unique, registry-enforced.
        QString displayName;   // a LABEL ONLY. An app can call itself anything.
        QString iconSource;
    };

    struct Resolution {
        Status status = None;
        QList<ProviderEntry> found;
    };

    // Injected so the registry stays dependency-free, and so a chooser can
    // never disagree with the sidebar about what an app is called or how it
    // looks — both read from the same place.
    using LabelFn = std::function<QString(const QString& moduleName)>;
    using IconFn  = std::function<QString(const QString& moduleName)>;

    explicit IntentRegistry(QObject* parent = nullptr);

    // `plugins` maps moduleName -> the plugin metadata basecamp already holds;
    // only "installDir" and "type" are read from it. Everything intent-related
    // comes from <installDir>/metadata.json, freshly read here.
    void rebuild(const QMap<QString, QVariantMap>& plugins,
                 const LabelFn& labelFor,
                 const IconFn& iconFor);

    // The shell has no installDir and no metadata.json, so its `uses` is
    // declared in code. Still a declaration: the broker checks it exactly as it
    // checks an app's.
    void registerShellUses(const QString& shellModuleName,
                           const QStringList& intents);

    // The same for `provides`, and the only legitimate source of a "logos.*"
    // name — any disk record claiming one is refused.
    void registerShellProvider(const QString& shellModuleName,
                               const QStringList& intents,
                               const QString& displayName,
                               const QString& iconSource);

    // Limit who may request `intent`. For capabilities where no third party has
    // a legitimate use — removing another app's package, say — attribution in
    // the dialog is not enough: the right answer to the prompt is always no, so
    // the prompt should not exist. Survives rebuild(); it is code-declared
    // policy, not something read off disk.
    void restrictIntentToRequesters(const QString& intent,
                                    const QStringList& requesters);

    // True when `intent` is unrestricted, or `requesterName` is on its list.
    bool requesterAllowed(const QString& intent,
                          const QString& requesterName) const;

    Resolution resolve(const QString& intent) const;

    // What a provider says an intent's payload should contain, from its own
    // metadata.json. A property of the (provider, intent) pair, not the intent —
    // two providers may describe one intent differently and there is no
    // per-intent schema to appeal to. Empty means "not described", never "takes
    // nothing".
    //
    // Each entry: { name, type, required, description }.
    QVariantList paramsSpecFor(const QString& moduleName,
                               const QString& intent) const;

    // ── Installable providers — a SEPARATE table, deliberately ──────────
    //
    // What the CATALOG says could service an intent if installed. Sourced from
    // AppsModel's ProvidesRole, since an uninstalled package has no install dir
    // and so no metadata.json for rebuild() to read.
    //
    // Kept apart from the installed table on purpose: resolve() must never return
    // one of these, and merging the two would make dispatching to a package that
    // is not on disk an easy mistake to make. It answers only "could the user
    // install something for this?" — a question the SHELL asks, never an app.
    void setInstallableProviders(const QMap<QString, QStringList>& byModuleName);

    // Packages the catalog says provide `intent` and that are not installed.
    // Sorted by module name, for the same reason resolve() sorts: a
    // non-deterministic prompt is a flaky prompt.
    QStringList installableProvidersFor(const QString& intent) const;

    // True for the host's own module name. The registry is the single source of
    // this fact — it already holds the name to refuse disk records claiming to
    // be the shell — so nothing else keeps a copy that could drift.
    bool isShellProvider(const QString& moduleName) const;

    bool declaresUse(const QString& moduleName, const QString& intent) const;
    bool declaresProvide(const QString& moduleName, const QString& intent) const;

    // Everything skipped or refused during the last rebuild. Never fatal, never
    // surfaced to an app — but a capability that silently fails to register is
    // otherwise very hard to diagnose from outside.
    QStringList diagnostics() const;

signals:
    void changed();

private:
    void reset();
    void ingestRecord(const QString& moduleName,
                      const QVariantMap& metadata,
                      const LabelFn& labelFor,
                      const IconFn& iconFor);

    QMap<QString, QStringList> m_provides;   // moduleName -> intents
    QMap<QString, QStringList> m_uses;       // moduleName -> intents
    QMap<QString, ProviderEntry> m_entries;  // moduleName -> label/icon
    QString m_shellModuleName;

    // intent -> module names that could provide it once installed. Never
    // consulted by resolve().
    QHash<QString, QStringList> m_installable;

    // intent -> requesters permitted to ask. Absent = unrestricted. Not cleared
    // by reset(), like m_shellModuleName: declared in code, not read from disk.
    QHash<QString, QStringList> m_restrictedIntents;

    // (moduleName, intent) -> [{name,type,required,description}]
    QHash<QString, QVariantList> m_paramsSpec;
    QStringList m_diagnostics;
};
