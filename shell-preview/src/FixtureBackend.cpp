#include "FixtureBackend.h"

#include <QDebug>
#include <QJsonArray>

FixtureBackend::FixtureBackend(const QJsonObject& fixture, QObject* parent)
    : QObject(parent)
    , m_fixture(fixture)
    , m_uiModules(moduleInstanceRoleNames(), this)
    , m_coreModules(moduleInstanceRoleNames(), this)
    , m_apps(appsRoleNames(), this)
{
    m_uiModules.setRows(m_fixture.value("uiModules").toArray().toVariantList());
    m_coreModules.setRows(m_fixture.value("coreModules").toArray().toVariantList());
    m_apps.setRows(m_fixture.value("apps").toArray().toVariantList());
}

int FixtureBackend::currentActiveSectionIndex() const { return m_sectionIndex; }
QAbstractItemModel* FixtureBackend::uiModulesModel() const { return const_cast<FixtureListModel*>(&m_uiModules); }
QAbstractItemModel* FixtureBackend::coreModulesModel() const { return const_cast<FixtureListModel*>(&m_coreModules); }
QAbstractItemModel* FixtureBackend::appsModel() const { return const_cast<FixtureListModel*>(&m_apps); }
QVariantList FixtureBackend::requiredPackages() const { return m_fixture.value("requiredPackages").toArray().toVariantList(); }
QVariantList FixtureBackend::launcherApps() const { return m_fixture.value("launcherApps").toArray().toVariantList(); }
QString FixtureBackend::currentVisibleApp() const { return m_currentVisibleApp; }
QStringList FixtureBackend::loadingModules() const { QStringList o; for (const auto& v : m_fixture.value("loadingModules").toArray()) o << v.toString(); return o; }
QString FixtureBackend::buildVersion() const { return m_fixture.value("buildVersion").toString(); }
bool FixtureBackend::isPortableBuild() const { return m_fixture.value("isPortableBuild").toBool(); }
bool FixtureBackend::isMockBackend() const { return m_fixture.value("isMockBackend").toBool(); }
QVariantList FixtureBackend::buildCommits() const { return m_fixture.value("buildCommits").toArray().toVariantList(); }
QVariantList FixtureBackend::repositories() const { return m_fixture.value("repositories").toArray().toVariantList(); }
bool FixtureBackend::repositoriesLoading() const { return m_fixture.value("repositoriesLoading").toBool(); }
bool FixtureBackend::appsLoading() const { return m_fixture.value("appsLoading").toBool(); }
bool FixtureBackend::modulesLoading() const { return m_fixture.value("modulesLoading").toBool(); }

QString FixtureBackend::displayNameFor(const QString& moduleName) const
{
    for (const auto& v : m_fixture.value("apps").toArray()) {
        const auto o = v.toObject();
        if (o.value("name").toString() == moduleName)
            return o.value("displayName").toString();
    }
    return moduleName;
}

void FixtureBackend::uninstallApp(const QString& name, const QString& repositoryUrl)
{
    Q_UNUSED(name); Q_UNUSED(repositoryUrl);
    qInfo() << "FixtureBackend: uninstallApp — fixture build, no effect";
}

void FixtureBackend::confirmUnloadCascade(const QString& moduleName)
{
    Q_UNUSED(moduleName);
    qInfo() << "FixtureBackend: confirmUnloadCascade — fixture build, no effect";
}

void FixtureBackend::confirmUninstallCascade(const QString& moduleName)
{
    Q_UNUSED(moduleName);
    qInfo() << "FixtureBackend: confirmUninstallCascade — fixture build, no effect";
}

void FixtureBackend::confirmUninstallMultiCascade(const QStringList& moduleNames)
{
    Q_UNUSED(moduleNames);
    qInfo() << "FixtureBackend: confirmUninstallMultiCascade — fixture build, no effect";
}

void FixtureBackend::cancelMultiUninstall(const QStringList& moduleNames)
{
    Q_UNUSED(moduleNames);
    qInfo() << "FixtureBackend: cancelMultiUninstall — fixture build, no effect";
}

void FixtureBackend::cancelPendingAction(const QString& moduleName)
{
    Q_UNUSED(moduleName);
    qInfo() << "FixtureBackend: cancelPendingAction — fixture build, no effect";
}

void FixtureBackend::cancelPendingUninstallApp(const QString& name)
{
    Q_UNUSED(name);
    qInfo() << "FixtureBackend: cancelPendingUninstallApp — fixture build, no effect";
}

void FixtureBackend::confirmInstallGate(const QString& name)
{
    Q_UNUSED(name);
    qInfo() << "FixtureBackend: confirmInstallGate — fixture build, no effect";
}

void FixtureBackend::cancelInstallGate(const QString& name)
{
    Q_UNUSED(name);
    qInfo() << "FixtureBackend: cancelInstallGate — fixture build, no effect";
}

void FixtureBackend::openApp(const QString& name, const QString& repositoryUrl, const QVariantMap& versionPins, bool allowFastLaunch)
{
    Q_UNUSED(name); Q_UNUSED(repositoryUrl); Q_UNUSED(versionPins); Q_UNUSED(allowFastLaunch);
    qInfo() << "FixtureBackend: openApp — fixture build, no effect";
}

void FixtureBackend::confirmCatalogInstall(const QString& name, const QString& repositoryUrl, const QVariantMap& versionPins)
{
    Q_UNUSED(name); Q_UNUSED(repositoryUrl); Q_UNUSED(versionPins);
    qInfo() << "FixtureBackend: confirmCatalogInstall — fixture build, no effect";
}

void FixtureBackend::notifyAddApplicationDialogClosed()
{
    qInfo() << "FixtureBackend: notifyAddApplicationDialogClosed — fixture build, no effect";
}

void FixtureBackend::refreshCoreModules()
{
    qInfo() << "FixtureBackend: refreshCoreModules — fixture build, no effect";
}

QString FixtureBackend::getCoreModuleMethods(const QString& moduleName)
{
    Q_UNUSED(moduleName);
    qInfo() << "FixtureBackend: getCoreModuleMethods — fixture build, no effect";
    return {};
}

QString FixtureBackend::getCoreModuleEvents(const QString& moduleName)
{
    Q_UNUSED(moduleName);
    qInfo() << "FixtureBackend: getCoreModuleEvents — fixture build, no effect";
    return {};
}

QString FixtureBackend::callCoreModuleMethod(const QString& moduleName, const QString& methodName, const QString& argsJson)
{
    Q_UNUSED(moduleName); Q_UNUSED(methodName); Q_UNUSED(argsJson);
    qInfo() << "FixtureBackend: callCoreModuleMethod — fixture build, no effect";
    return {};
}

void FixtureBackend::refreshUiModules()
{
    qInfo() << "FixtureBackend: refreshUiModules — fixture build, no effect";
}

void FixtureBackend::refreshRepositories()
{
    qInfo() << "FixtureBackend: refreshRepositories — fixture build, no effect";
}

void FixtureBackend::refreshAppCatalog()
{
    qInfo() << "FixtureBackend: refreshAppCatalog — fixture build, no effect";
}

void FixtureBackend::addRepository(const QString& url)
{
    Q_UNUSED(url);
    qInfo() << "FixtureBackend: addRepository — fixture build, no effect";
}

void FixtureBackend::removeRepository(const QString& url)
{
    Q_UNUSED(url);
    qInfo() << "FixtureBackend: removeRepository — fixture build, no effect";
}

void FixtureBackend::setRepositoryEnabled(const QString& url, bool enabled)
{
    Q_UNUSED(url); Q_UNUSED(enabled);
    qInfo() << "FixtureBackend: setRepositoryEnabled — fixture build, no effect";
}

void FixtureBackend::setCurrentVisibleApp(const QString& name)
{
    if (m_currentVisibleApp == name) return;
    m_currentVisibleApp = name;
    emit currentVisibleAppChanged();
}

void FixtureBackend::setCurrentActiveSectionIndex(int index)
{
    if (m_sectionIndex == index) return;
    m_sectionIndex = index;
    emit currentActiveSectionIndexChanged();
}

void FixtureBackend::loadUiModule(const QString& moduleName)
{
    Q_UNUSED(moduleName);
    qInfo() << "FixtureBackend: loadUiModule — fixture build, no effect";
}

void FixtureBackend::unloadUiModule(const QString& moduleName)
{
    Q_UNUSED(moduleName);
    qInfo() << "FixtureBackend: unloadUiModule — fixture build, no effect";
}

void FixtureBackend::loadCoreModule(const QString& moduleName)
{
    Q_UNUSED(moduleName);
    qInfo() << "FixtureBackend: loadCoreModule — fixture build, no effect";
}

void FixtureBackend::unloadCoreModule(const QString& moduleName)
{
    Q_UNUSED(moduleName);
    qInfo() << "FixtureBackend: unloadCoreModule — fixture build, no effect";
}

void FixtureBackend::onAppLauncherClicked(const QString& appName)
{
    Q_UNUSED(appName);
    qInfo() << "FixtureBackend: onAppLauncherClicked — fixture build, no effect";
}
