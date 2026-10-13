#ifndef LOGOS_BASECAMP_STANDALONE_MODE_H
#define LOGOS_BASECAMP_STANDALONE_MODE_H

#include "utils/HostProfile.h"

#include <QString>

namespace LogosBasecamp {

// Builds the dev-host profile from `--module` / `--modules-dir` / `--qml-source`.

struct StandaloneResolution {
    bool ok = true;
    QString error;
};

StandaloneResolution addStandaloneModule(HostProfile& profile, const QString& path);
StandaloneResolution addStandaloneModulesDir(HostProfile& profile, const QString& path);
void applyStandaloneCapabilities(HostProfile& profile);
StandaloneResolution addStandaloneQmlSource(HostProfile& profile, const QString& arg);

} // namespace LogosBasecamp

#endif // LOGOS_BASECAMP_STANDALONE_MODE_H
