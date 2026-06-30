#pragma once

#include "InstallEnums.h"

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class InstallRegistry;

// ModuleModel — single source of truth for catalog rows, installed UI plugins,
// and known core modules across Basecamp views.
class ModuleModel : public QAbstractListModel {
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
        ColorRole,               // manifest/catalog accent color; "" → AppColors hash
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
        IsLoadedRole,
        IsLoadingRole,
        IsMainUiRole,
        IconPathRole,            // file:// or qrc path for sidebar / modules tab
        HasMissingDepsRole,
        CpuRole,                 // core-module CPU % string
        MemoryRole,              // core-module memory MB string
        IsUiPluginRecordRole,    // row seeded from installed UI plugin scan
        IsCoreModuleRecordRole,  // row seeded from liblogos known-modules list
    };
    Q_ENUM(Roles)

    explicit ModuleModel(QObject* parent = nullptr);

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
    void setDisplayName(const QString& name, const QString& displayName);
    void setIconUrl(const QString& name, const QString& iconUrl);
    void setMissingDeps(const QString& name, const QStringList& missing);
    void setInstallRegistry(InstallRegistry* installRegistry);

    // Insert or update a row for an installed-only module (UI plugin or core).
    void seedInstalledOnly(const QString& name,
                           const QString& type,
                           const QVariantMap& fields);
    void setRoleByName(const QString& name, int role, const QVariant& value);

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
        QString color;
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

        bool    isLoaded = false;
        bool    isLoading = false;
        bool    isMainUi = false;
        QString iconPath;
        QString cpu;
        QString memory;
        bool    isUiPluginRecord = false;
        bool    isCoreModuleRecord = false;
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
