#include "UIPluginPresenter.h"

#include "UIPluginManager.h"

UIPluginPresenter::UIPluginPresenter(UIPluginManager* uiPluginManager,
                                     QObject* parent)
    : QObject(parent)
    , m_uiPluginManager(uiPluginManager)
{
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



