#pragma once

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
class AppsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QStringList categories READ categories NOTIFY categoriesChanged)
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        RepositoryUrlRole,
        DisplayNameRole,
        DescriptionRole,
        CategoryRole,
        TypeRole,                // "ui_qml" | "core" 
        IconUrlRole,             // file:// URL when installed; "" otherwise
        VersionsRole,            // QVariantList — all known catalog versions
        DependenciesRole,        // QVariantList — direct deps of versions[0]'s manifest,
                                 //   normalized to [{name, version}, ...]. version "" = no constraint.
        InstalledVersionRole,    // "" when not installed
        LatestVersionRole,       // versions[0].version
        HasUpdateRole,           // installedVersion != "" && installed != latest
        IsInstalledRole,         // installedVersion != ""
        MissingDepsRole,         // QStringList of dep names whose LGX isn't on
                                 //   disk (sourced from
                                 //   PackageCoordinator::m_missingDepsByModule).
                                 //   Empty when the install is complete.
        InstallStatusRole,       // InstallStatus enum — per-row catalog-vs-disk
                                 //   state (Install/Launch/Upgrade/Downgrade/
                                 //   Reinstall in QML terms). PMUI mirror.
        InstallTypeRole,         // "embedded" | "user" | ""
        ActionRole,            
        ToVersionRole,
        IsTopLevelRole,
        ResolverErrorRole,
        InstallStageRole,        // InstallStage::Value (int) — see InstallEnums.h
        InstallErrorRole,        // failure message when InstallStage == Failed
    };
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

    void replaceCatalog(const QVariantList& catalogRows);

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
        QVariantList versions;
        QString latestVersion;       // computed from versions[0].version
        QVariantList dependencies;

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
