#pragma once

#include "BasecampModelRoles.h"
#include "InstallEnums.h"

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class InstallRegistry;

// AppsModel — the single source of truth for every package the App Manager
// (and Modules tab) cares about.
class AppsModel : public QAbstractListModel, public AppsModelRoles {
    Q_OBJECT
    Q_PROPERTY(QStringList categories READ categories NOTIFY categoriesChanged)
public:
    Q_ENUM(Roles)

    explicit AppsModel(QObject* parent = nullptr);

    // ── QAbstractListModel
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QStringList categories() const;

signals:
    void categoriesChanged();

public:

    // True once the manifest guarantees a conforming icon (>= 0.4.0). Static
    // and public so UIPluginManager's sidebar path uses the SAME rule — this
    // comparison must exist once. Mirrors Manifest::requiresIconContract().
    static bool supportsFullBleedIcon(const QString& manifestVersion);


    void replaceCatalog(const QVariantList& catalogRows);

    // Append synthetic rows for installed packages that have no catalog row.
    // Marked by empty repositoryUrl
    void mergeLocalOnlyInstalled(const QVariantList& installedPackages);

    void markInstalled(const QString& name,
                       const QString& installedVersion,
                       const QString& installedHash = {});
    void replaceInstalledSet(const QHash<QString, QString>& versionByName,
                             const QHash<QString, QString>& hashByName);

    void setInstallType(const QString& name, const QString& installType);
    void setIconUrl(const QString& name, const QString& iconUrl);
    void setMissingDeps(const QString& name, const QStringList& missing);
    void setInstallRegistry(InstallRegistry* installRegistry);

    void beginBulkInstalledUpdate();
    void endBulkInstalledUpdate();

    struct ResolverRow {
        QString name;
        QString repositoryUrl;
        QString action;
        QString toVersion;   
        bool    isTopLevel = false;
        QString resolverError; 
    };
    void setResolverOverlay(const QList<ResolverRow>& rows);
    void clearResolverOverlay();

    QVariantMap rowDataByName(const QString& name,
                              const QString& repositoryUrl = {}) const;

private:
    int rowOf(const QString& name,
              const QString& repositoryUrl = {}) const;
    QVariantMap rowData(int row) const;

    struct Row {
        // Identity
        QString name;
        QString repositoryUrl;

        // Catalog metadata
        QString displayName;
        QString description;
        QString category;
        QString type;
        QString iconUrl;
        bool supportsFullBleedIcon = false;
        QVariantList versions;
        QString latestVersion;       // computed from versions[0].version
        QVariantList dependencies;
        QStringList provides;

        // On-disk state
        QString installedVersion;
        QString installedHash;
        QString installType;
        QStringList missingDeps;
        InstallStatus::Value installStatus = InstallStatus::NotInstalled;

        // Resolver overlay (per dialog session). Live install state lives
        // on m_installRegistry — see setInstallRegistry. Per-row InstallStageRole /
        // InstallErrorRole / ActionRole derive from there at read time.
        QString action;
        QString toVersion;
        bool    isTopLevel = false;
        QString resolverError;
    };

    static QString key(const QString& repo, const QString& name);


    void recomputeVersionDerivedFields(Row& r);
    void recomputeInstallStatus(Row& r);

    QList<Row>          m_rows;
    QHash<QString, int> m_indexByKey;     // (repo + "\n" + name) → row index
    QMultiHash<QString, int> m_indicesByName;
    bool m_inBulkInstalledUpdate = false;

    // Source of truth for in-flight install state
    QPointer<InstallRegistry> m_installRegistry;
};
