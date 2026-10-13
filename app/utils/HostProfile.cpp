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

} // namespace LogosBasecamp
