#pragma once

#include <QString>
#include <QStringList>

class IntentRegistry;

namespace ShellIntents {

inline const QStringList kPackageConfirmIntents = {
    QStringLiteral("basecamp.packages.confirm_install"),
    QStringLiteral("basecamp.packages.confirm_uninstall"),
    QStringLiteral("basecamp.packages.confirm_upgrade"),
};

// Of those, the ones no third party has a legitimate reason to raise.
inline const QStringList kRestrictedToPackageManagerUi = {
    QStringLiteral("basecamp.packages.confirm_uninstall"),
    QStringLiteral("basecamp.packages.confirm_upgrade"),
};

// PURE NAVIGATION, and hand-offs to a one. `ok` means "you are there", not "we
// are done", so the broker must not bounce the user back out of a destination
// it was asked to take them to.
inline const QStringList kNavigationIntents = {
    QStringLiteral("basecamp.repositories.manage"),
    QStringLiteral("basecamp.settings.open"),
    QStringLiteral("basecamp.apps.open"),
    QStringLiteral("basecamp.apps.launch"),
    QStringLiteral("basecamp.packages.open"),
};

// Bring a named app forward: `{ "app": "wallet_ui" }`.
inline const QString kAppLaunchIntent = QStringLiteral("basecamp.apps.launch");
inline const QString kPackageManagerAppName =
    QStringLiteral("package_manager_ui");
inline const QString kPackagesOpenIntent =
    QStringLiteral("basecamp.packages.open");
inline const QString kAppLaunchParam  = QStringLiteral("app");

// Which of the shell's own capabilities a URL clicked outside Basecamp may
// reach. A SUBSET of what the shell provides, and deliberately not all of it:
// every entry here is something a web page can cause, so the confirm intents —
// which gate installing and removing packages — are absent and must stay so.
//
// SPELLED OUT, NOT `= kNavigationIntents`. The alias was the bug: because the
// broker skips the chooser when the shell is the provider (IntentBroker's
// `isShellProvider(only)` branch), this list is the entire boundary between a
// web page and a shell capability — there is no consent step behind it. As an
// alias, adding a navigation intent for an internal reason silently published
// it to every browser, in one line, with nothing at the edit site saying so.
//
// Adding an entry here is a decision to let any web page cause it, unprompted.
inline const QStringList kWebReachableIntents = {
    QStringLiteral("basecamp.repositories.manage"),
    QStringLiteral("basecamp.settings.open"),
    QStringLiteral("basecamp.apps.open"),
    QStringLiteral("basecamp.apps.launch"),
    QStringLiteral("basecamp.packages.open"),
};

// Declare the shell's provides, hand-offs, uses and requester restrictions.
void registerWith(IntentRegistry* registry,
                  const QString& shellModuleName,
                  const QString& displayName,
                  const QString& iconSource);

} // namespace ShellIntents
