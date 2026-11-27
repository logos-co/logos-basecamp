#include "utils/HostProfile.h"

#include "DefaultApps.h"
#include "ShellSections.h"

namespace LogosBasecamp {

QList<int> HostProfile::availableSections() const
{
    QList<int> sections{ShellSection::Workspace};
    // Standalone mode drops the catalogue, but an installer app under
    // development still gets its own section, where it lives in Basecamp.
    for (const int section : {int(ShellSection::AppManager), int(ShellSection::PackageManager)}) {
        const auto* app = logos::defaultapps::appInSection(section);
        if (packageCatalog || (app && appsToOpen.contains(app->app))) sections << section;
    }
    sections << ShellSection::Settings;
    return sections;
}

int HostProfile::sectionOfApp(const QString& app) const
{
    const int section = logos::defaultapps::sectionOf(app);
    return section >= 0 && availableSections().contains(section) ? section : -1;
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
