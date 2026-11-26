#pragma once

#include "ShellSections.h"

#include <QList>
#include <QString>
#include <QStringList>

// Basecamp's default apps, by slot: the role an app fills and what the role
// may do. Grants go by slot, never by a name spelled out elsewhere: the
// built-in apps, who may raise the confirm intents, who may call the package
// modules, who may hand Basecamp a local file and which app is a section all
// come from this table. Header-only, for the host and the shell alike.
namespace logos::defaultapps {

enum class Slot { Apps, Packages };

struct DefaultApp {
    Slot slot;
    QString app;
    // The ShellSection it is shown as, or -1 for none.
    int section = -1;
    // The restricted intents its slot may raise; no other app may.
    QStringList mayAsk;
    // May ask Basecamp to read a local .lgx it names.
    bool namesLocalFiles = false;
    // Its backend may call package_manager and package_downloader.
    bool callsPackageModules = false;
    // The intents Basecamp's own requests go to it for, when it provides
    // them, with no chooser: the welcome page's Install and "show all" go to
    // App Manager.
    QStringList defaultFor;
};

inline QString backendOf(const QString& app)
{
    return app + QStringLiteral("_backend");
}

inline const QList<DefaultApp>& table()
{
    static const QList<DefaultApp> rows = [] {
        const QStringList confirm{QStringLiteral("basecamp.packages.confirm_install"),
                                  QStringLiteral("basecamp.packages.confirm_uninstall"),
                                  QStringLiteral("basecamp.packages.confirm_upgrade")};
        return QList<DefaultApp>{
            {Slot::Apps, QStringLiteral("app_manager_ui"), ShellSection::AppManager, confirm,
             false, true, {QStringLiteral("packages.install"), QStringLiteral("apps.search")}},
            {Slot::Packages, QStringLiteral("package_manager_ui"), ShellSection::PackageManager, confirm,
             true, true, {QStringLiteral("packages.show")}},
        };
    }();
    return rows;
}

// The row `name` runs under: the app, or its backend. Names are identities
// here, compared exactly.
inline const DefaultApp* appNamed(const QString& name, const QList<DefaultApp>& rows = table())
{
    for (const DefaultApp& row : rows) {
        if (row.app == name || backendOf(row.app) == name) return &row;
    }
    return nullptr;
}

inline const DefaultApp* appInSlot(Slot slot, const QList<DefaultApp>& rows = table())
{
    for (const DefaultApp& row : rows) {
        if (row.slot == slot) return &row;
    }
    return nullptr;
}

inline const DefaultApp* appInSection(int section, const QList<DefaultApp>& rows = table())
{
    for (const DefaultApp& row : rows) {
        if (section >= 0 && row.section == section) return &row;
    }
    return nullptr;
}

// The ShellSection `app` is shown as, or -1.
inline int sectionOf(const QString& app, const QList<DefaultApp>& rows = table())
{
    for (const DefaultApp& row : rows) {
        if (row.app == app) return row.section;
    }
    return -1;
}

inline QStringList apps(const QList<DefaultApp>& rows = table())
{
    QStringList out;
    for (const DefaultApp& row : rows) out << row.app;
    return out;
}

// Every intent some slot is the only one allowed to raise.
inline QStringList restrictedIntents(const QList<DefaultApp>& rows = table())
{
    QStringList out;
    for (const DefaultApp& row : rows) {
        for (const QString& intent : row.mayAsk) {
            if (!out.contains(intent)) out << intent;
        }
    }
    return out;
}

// Who may raise `intent`: each slot's app, through its screen, and its
// backend, raising it itself.
inline QStringList requestersFor(const QString& intent, const QList<DefaultApp>& rows = table())
{
    QStringList out;
    for (const DefaultApp& row : rows) {
        if (row.mayAsk.contains(intent)) out << row.app << backendOf(row.app);
    }
    return out;
}

// The backends of the slots that may raise `intent`: who intents_module, which
// knows modules only, lets ask for it.
inline QStringList backendsFor(const QString& intent, const QList<DefaultApp>& rows = table())
{
    QStringList out;
    for (const DefaultApp& row : rows) {
        if (row.mayAsk.contains(intent)) out << backendOf(row.app);
    }
    return out;
}

// Who may call package_manager and package_downloader: Basecamp (`core`) and
// the backends of the slots allowed to.
inline QStringList packageModuleCallers(const QList<DefaultApp>& rows = table())
{
    QStringList out{QStringLiteral("core")};
    for (const DefaultApp& row : rows) {
        if (row.callsPackageModules) out << backendOf(row.app);
    }
    return out;
}

// The backend Basecamp's own request for `intent` goes to without a chooser
// when it is among the providers, or "".
inline QString defaultProviderFor(const QString& intent, const QList<DefaultApp>& rows = table())
{
    for (const DefaultApp& row : rows) {
        if (row.defaultFor.contains(intent)) return backendOf(row.app);
    }
    return {};
}

inline bool namesLocalFiles(const QString& requester, const QList<DefaultApp>& rows = table())
{
    const DefaultApp* row = appNamed(requester, rows);
    return row && row->namesLocalFiles;
}

}  // namespace logos::defaultapps
