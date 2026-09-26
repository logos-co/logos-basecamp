#pragma once

#include "FixtureModels.h"

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// The QObject the shell binds to, in place of MainUIBackend.
//
// Surface derived by enumerating what src/Basecamp/**.qml reads:
// 17 properties, 33 invokables, 6 slots. No Logos.
class FixtureBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(int currentActiveSectionIndex READ currentActiveSectionIndex WRITE setCurrentActiveSectionIndex NOTIFY currentActiveSectionIndexChanged)
    Q_PROPERTY(QAbstractItemModel* uiModulesModel READ uiModulesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* coreModulesModel READ coreModulesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* appsModel READ appsModel CONSTANT)
    Q_PROPERTY(QVariantList requiredPackages READ requiredPackages NOTIFY requiredPackagesChanged)
    Q_PROPERTY(QVariantList launcherApps READ launcherApps NOTIFY launcherAppsChanged)
    Q_PROPERTY(QString currentVisibleApp READ currentVisibleApp NOTIFY currentVisibleAppChanged)
    Q_PROPERTY(QStringList loadingModules READ loadingModules NOTIFY loadingModulesChanged)
    Q_PROPERTY(QString buildVersion READ buildVersion CONSTANT)
    Q_PROPERTY(bool isPortableBuild READ isPortableBuild CONSTANT)
    Q_PROPERTY(bool isMockBackend READ isMockBackend CONSTANT)
    Q_PROPERTY(QVariantList buildCommits READ buildCommits CONSTANT)
    Q_PROPERTY(QVariantList repositories READ repositories NOTIFY repositoriesChanged)
    Q_PROPERTY(bool repositoriesLoading READ repositoriesLoading NOTIFY repositoriesLoadingChanged)
    Q_PROPERTY(bool appsLoading READ appsLoading NOTIFY appsLoadingChanged)
    Q_PROPERTY(bool modulesLoading READ modulesLoading NOTIFY modulesLoadingChanged)
    Q_PROPERTY(QVariantMap peering READ peering NOTIFY peeringChanged)

public:
    explicit FixtureBackend(const QJsonObject& fixture, QObject* parent = nullptr);

    int currentActiveSectionIndex() const;
    QAbstractItemModel* uiModulesModel() const;
    QAbstractItemModel* coreModulesModel() const;
    QAbstractItemModel* appsModel() const;
    QVariantList requiredPackages() const;
    QVariantList launcherApps() const;
    QString currentVisibleApp() const;
    QStringList loadingModules() const;
    QString buildVersion() const;
    bool isPortableBuild() const;
    bool isMockBackend() const;
    QVariantList buildCommits() const;
    QVariantList repositories() const;
    bool repositoriesLoading() const;
    bool appsLoading() const;
    bool modulesLoading() const;
    QVariantMap peering() const;

    Q_INVOKABLE QString displayNameFor(const QString& moduleName) const;
    Q_INVOKABLE void uninstallApp(const QString& name, const QString& repositoryUrl = QString());
    Q_INVOKABLE void confirmUnloadCascade(const QString& moduleName);
    Q_INVOKABLE void confirmUninstallCascade(const QString& moduleName);
    Q_INVOKABLE void confirmUninstallMultiCascade(const QStringList& moduleNames);
    Q_INVOKABLE void cancelMultiUninstall(const QStringList& moduleNames);
    Q_INVOKABLE void cancelPendingAction(const QString& moduleName);
    Q_INVOKABLE void cancelPendingUninstallApp(const QString& name);
    Q_INVOKABLE void confirmInstallGate(const QString& name);
    Q_INVOKABLE void cancelInstallGate(const QString& name);
    Q_INVOKABLE void openApp(const QString& name, const QString& repositoryUrl, const QVariantMap& versionPins = QVariantMap(), bool allowFastLaunch = true);
    Q_INVOKABLE void confirmCatalogInstall(const QString& name, const QString& repositoryUrl, const QVariantMap& versionPins = QVariantMap());
    Q_INVOKABLE void notifyAddApplicationDialogClosed();
    Q_INVOKABLE void refreshCoreModules();
    Q_INVOKABLE QString getCoreModuleMethods(const QString& moduleName);
    Q_INVOKABLE QString getCoreModuleEvents(const QString& moduleName);
    Q_INVOKABLE QString callCoreModuleMethod(const QString& moduleName, const QString& methodName, const QString& argsJson);
    Q_INVOKABLE void refreshUiModules();
    Q_INVOKABLE void refreshRepositories();
    Q_INVOKABLE void refreshAppCatalog();
    Q_INVOKABLE void addRepository(const QString& url);
    Q_INVOKABLE void removeRepository(const QString& url);
    Q_INVOKABLE void setRepositoryEnabled(const QString& url, bool enabled);
    Q_INVOKABLE void refreshPeering();
    Q_INVOKABLE void setPeeringEnabled(bool enabled);
    Q_INVOKABLE void linkLocalDaemon(const QString& invitePath);
    Q_INVOKABLE void pairWithPeer(const QString& host, int port);
    Q_INVOKABLE void openPeerPairingWindow(int seconds);
    Q_INVOKABLE void confirmPeerPairing(const QString& id);
    Q_INVOKABLE void rejectPeerPairing(const QString& id);
    Q_INVOKABLE void removePeer(const QString& peer);
    Q_INVOKABLE void fetchPeerExports(const QString& peer);
    Q_INVOKABLE void importPeerModule(const QString& peer, const QString& module, bool events);
    Q_INVOKABLE void removePeerImport(const QString& name);

    // Not QML-bound: the shell reaches this through IShellHost.
    void setCurrentVisibleApp(const QString& name);

public slots:
    void setCurrentActiveSectionIndex(int index);
    void loadUiModule(const QString& moduleName);
    void unloadUiModule(const QString& moduleName);
    void loadCoreModule(const QString& moduleName);
    void unloadCoreModule(const QString& moduleName);
    void onAppLauncherClicked(const QString& appName);

signals:
    void appsLoadingChanged();
    void currentActiveSectionIndexChanged();
    void currentVisibleAppChanged();
    void launcherAppsChanged();
    void loadingModulesChanged();
    void modulesLoadingChanged();
    void peeringChanged();
    void peeringOperationCompleted(const QString& operation, bool success, const QString& error);
    void peerExportsFetched(const QString& peer, const QVariantList& exports);
    void peerPairingRequested(const QVariantMap& request);
    void repositoriesChanged();
    void repositoriesLoadingChanged();
    void requiredPackagesChanged();

private:
    QJsonObject      m_fixture;
    FixtureListModel m_uiModules;
    FixtureListModel m_coreModules;
    FixtureListModel m_apps;
    int              m_sectionIndex = 0;
    QString          m_currentVisibleApp;
};
