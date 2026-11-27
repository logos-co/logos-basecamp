#include "UIPluginPresenter.h"

#include "ShellSections.h"
#include "UIPluginManager.h"
#include "utils/Frontmost.h"

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
    // No provider means we cannot tell which section is up. Answer false: the
    // only thing this gates is a navigation, and declining to move is the safe
    // direction.
    if (!m_uiPluginManager || !m_sectionProvider)
        return false;
    return logos::isFrontmost(appName, m_uiPluginManager->sectionOfApp(appName), m_sectionProvider(),
                              ShellSection::Workspace, m_uiPluginManager->currentVisibleApp());
}

bool UIPluginPresenter::anyDialogOpen() const
{
    return m_dialogProbe ? m_dialogProbe() : false;
}



