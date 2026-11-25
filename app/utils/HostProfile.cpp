#include "utils/HostProfile.h"

#include "ShellSections.h"

namespace LogosBasecamp {

QList<int> HostProfile::availableSections() const
{
    QList<int> sections{ShellSection::Workspace};
    if (packageCatalog) sections << ShellSection::AppManager;
    // Standalone mode drops the catalogue, but a package_manager_ui under
    // development still gets its own section, where it lives in Basecamp.
    if (packageCatalog || appsToOpen.contains(QStringLiteral("package_manager_ui")))
        sections << ShellSection::PackageManager;
    sections << ShellSection::Settings;
    return sections;
}

bool HostProfile::packageManagerUiIsSection() const
{
    return availableSections().contains(ShellSection::PackageManager);
}

QString intentsAttentionFor(const QString& requested, bool developerBuild, QString* warning)
{
    const QString fallback = HostProfile().intentsAttention;
    if (requested.isEmpty()) return fallback;
    if (requested == QLatin1String("immediate") && !developerBuild) {
        if (warning)
            *warning = QStringLiteral("--intents-attention immediate is for developer builds; using %1").arg(fallback);
        return fallback;
    }
    if (requested == QLatin1String("bundled") || requested == QLatin1String("interactive")
        || requested == QLatin1String("immediate"))
        return requested;
    if (warning)
        *warning = QStringLiteral("--intents-attention: unknown value %1 - using %2").arg(requested, fallback);
    return fallback;
}

} // namespace LogosBasecamp
