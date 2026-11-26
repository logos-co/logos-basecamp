#include "UIPluginPresenter.h"

#include "ShellSections.h"
#include "UIPluginManager.h"

UIPluginPresenter::UIPluginPresenter(UIPluginManager* uiPluginManager,
                                     SectionFn sectionProvider,
                                     QObject* parent)
    : QObject(parent)
    , m_uiPluginManager(uiPluginManager)
    , m_sectionProvider(std::move(sectionProvider))
{
}

void UIPluginPresenter::setDialogProbe(std::function<bool()> probe)
{
    m_dialogProbe = std::move(probe);
}

bool UIPluginPresenter::isAppLoaded(const QString& appName) const
{
    return m_uiPluginManager && m_uiPluginManager->isUiAppLoaded(appName);
}

void UIPluginPresenter::ensureAppLoaded(const QString& appName)
{
    if (!m_uiPluginManager) return;
    m_uiPluginManager->onAppLauncherClicked(appName);
}

void UIPluginPresenter::presentApp(const QString& appName)
{
    if (!m_uiPluginManager) return;
    m_uiPluginManager->activateApp(appName);
}

// BEING THE CURRENT APP AND BEING ON SCREEN ARE DIFFERENT QUESTIONS.
// currentVisibleApp is not cleared when the user switches to Settings — the
// sidebar deliberately keeps highlighting the app they were last in — so the
// name check alone would report a backgrounded app as frontmost and let a
// request drag someone out of Settings.
bool UIPluginPresenter::isAppFrontmost(const QString& appName) const
{
    if (!m_uiPluginManager || appName.isEmpty())
        return false;
    if (m_uiPluginManager->currentVisibleApp() != appName)
        return false;

    // No provider means we cannot tell which section is up. Answer false: the
    // only thing this gates is a navigation, and declining to move is the safe
    // direction.
    if (!m_sectionProvider)
        return false;

    const int section = m_sectionProvider();

    // A default app with a section is hoisted into the content stack rather
    // than docked, so "frontmost" means its section is selected — where the
    // host has that section (see HostProfile).
    const int appSection = m_uiPluginManager ? m_uiPluginManager->sectionOfApp(appName) : -1;
    if (appSection >= 0)
        return section == appSection;

    return section == ShellSection::Workspace;
}

bool UIPluginPresenter::anyDialogOpen() const
{
    return m_dialogProbe ? m_dialogProbe() : false;
}



