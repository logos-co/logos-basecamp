#include "ShellIntents.h"

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

    for (const QString& destructive : kRestrictedToPackageManagerUi)
        registry->restrictIntentToRequesters(
            destructive, { QStringLiteral("package_manager_ui") });
}

QJsonObject moduleDeclaration(const QString& shellModuleName,
                              const QString& displayName,
                              const QString& iconSource,
                              const QString& linkRequesterName)
{
    QJsonObject restricted;
    for (const QString& confirm : kPackageConfirmIntents)
        restricted.insert(confirm, QJsonArray{kPackageManagerAppName + QStringLiteral("_backend")});
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
