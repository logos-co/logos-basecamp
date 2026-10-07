#include "utils/HostProfile.h"

#include "ShellSections.h"

namespace LogosBasecamp {

QList<int> HostProfile::availableSections() const
{
    QList<int> sections{ShellSection::Workspace};
    if (packageCatalog) sections << ShellSection::AppManager << ShellSection::PackageManager;
    sections << ShellSection::Settings;
    return sections;
}

} // namespace LogosBasecamp
