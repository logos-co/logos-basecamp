#pragma once

#include <QString>

// Whether `app` is the one on screen. Header-only, so it is under test.
namespace logos {

// `appSection`: the section `app` is shown as (HostProfile::sectionOfApp),
// or -1 for an app docked in the workspace; `section`: the one selected;
// `currentVisibleApp`: the workspace app last shown, kept while another
// section is up.
inline bool isFrontmost(const QString& app, int appSection, int section, int workspaceSection,
                        const QString& currentVisibleApp)
{
    if (app.isEmpty()) return false;
    // A section app is up when its section is, whichever workspace app was
    // shown last.
    if (appSection >= 0) return section == appSection;
    return currentVisibleApp == app && section == workspaceSection;
}

}  // namespace logos
