#include "ShellIntents.h"

#include "DefaultApps.h"
#include "IntentRegistry.h"

#include <QJsonArray>

namespace ShellIntents {

void registerWith(IntentRegistry* registry,
                  const QString& shellModuleName,
                  const QString& displayName,
                  const QString& iconSource)
{
    if (!registry) return;

    registry->registerShellUses(shellModuleName, kShellUses);

    QStringList provides = kNavigationIntents;
    provides += kPackageConfirmIntents;

    registry->registerShellProvider(shellModuleName, provides,
                                    kNavigationIntents, displayName, iconSource);

    for (const QString& intent : logos::defaultapps::restrictedIntents())
        registry->restrictIntentToRequesters(intent, logos::defaultapps::requestersFor(intent));
}

QJsonObject moduleDeclaration(const QString& shellModuleName,
                              const QString& displayName,
                              const QString& iconSource,
                              const QString& linkRequesterName)
{
    QStringList intents = kPackageConfirmIntents;
    for (const QString& intent : logos::defaultapps::restrictedIntents()) {
        if (!intents.contains(intent)) intents << intent;
    }
    QJsonObject restricted;
    for (const QString& intent : intents) {
        QStringList backends = logos::defaultapps::backendsFor(intent);
        // The module takes no empty list: the shell, which never asks.
        if (backends.isEmpty()) backends << shellModuleName;
        restricted.insert(intent, QJsonArray::fromStringList(backends));
    }
    return QJsonObject{
        {QStringLiteral("name"), shellModuleName},
        {QStringLiteral("reservedPrefix"), QStringLiteral("basecamp.")},
        {QStringLiteral("uses"), QJsonArray::fromStringList(kShellUses)},
        {QStringLiteral("provides"), QJsonArray::fromStringList(kNavigationIntents + kPackageConfirmIntents)},
        {QStringLiteral("handoff"), QJsonArray::fromStringList(kNavigationIntents)},
        {QStringLiteral("displayName"), displayName},
        {QStringLiteral("iconSource"), iconSource},
        {QStringLiteral("restricted"), restricted},
        {QStringLiteral("link"), QJsonObject{
             {QStringLiteral("name"), linkRequesterName},
             {QStringLiteral("webIntents"), QJsonArray::fromStringList(kWebReachableIntents)}}},
    };
}

} // namespace ShellIntents
