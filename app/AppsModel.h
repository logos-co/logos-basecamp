#pragma once

#include "BasecampModelRoles.h"
#include "InstallEnums.h"

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// AppsModel — the catalogue and what is installed, for the welcome page's
// search, the intent install offers and provider details.
class AppsModel : public QAbstractListModel, public AppsModelRoles {
    Q_OBJECT
public:
    Q_ENUM(Roles)

    enum NotAvailableReason {
        Available           = 0,
        NoVariantsPublished = 1,   // nothing offered — installable nowhere
        BuildFlavorMismatch = 2,   // this OS/arch IS offered, wrong flavor
        PlatformMismatch    = 3,   // OS/arch not offered at all
    };
    Q_ENUM(NotAvailableReason)

    explicit AppsModel(QObject* parent = nullptr);

    // ── QAbstractListModel
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // True once the manifest guarantees a conforming icon (>= 0.4.0). Static
    // and public so UIPluginManager's sidebar path uses the SAME rule — this
    // comparison must exist once. Mirrors Manifest::requiresIconContract().
    static bool supportsFullBleedIcon(const QString& manifestVersion);


    void replaceCatalog(const QVariantList& catalogRows);
    void setValidVariants(const QStringList& variants);
    QStringList validVariants() const { return m_validVariants; }

    // Append synthetic rows for installed packages that have no catalog row.
    // Marked by empty repositoryUrl
    void mergeLocalOnlyInstalled(const QVariantList& installedPackages);

    void markInstalled(const QString& name,
                       const QString& installedVersion,
                       const QString& installedHash = {});
    void replaceInstalledSet(const QHash<QString, QString>& versionByName,
                             const QHash<QString, QString>& hashByName);

    void setInstallType(const QString& name, const QString& installType);
    // manifestVersion is not optional: the icon and its full-bleed eligibility
    // come from the same manifest, so updating one without the other leaves a
    // 0.4.0+ icon rendering inset. Taking both makes that state unreachable.
    void setIconUrl(const QString& name,
                    const QString& iconUrl,
                    const QString& manifestVersion);
    void setMissingDeps(const QString& name, const QStringList& missing);

    void beginBulkInstalledUpdate();
    void endBulkInstalledUpdate();

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
        QStringList webProvides;   // provides entries with "web": true
        bool hasInstallableVersion = true;
        int  notAvailableReason = Available;   // NotAvailableReason

        // On-disk state
        QString installedVersion;
        QString installedHash;
        QString installType;
        QStringList missingDeps;
        InstallStatus::Value installStatus = InstallStatus::NotInstalled;
    };

    static QString key(const QString& repo, const QString& name);


    void recomputeVersionDerivedFields(Row& r);
    void recomputeInstallStatus(Row& r);

    QStringList         m_validVariants;
    QList<Row>          m_rows;
    QHash<QString, int> m_indexByKey;     // (repo + "\n" + name) → row index
    QMultiHash<QString, int> m_indicesByName;
    bool m_inBulkInstalledUpdate = false;
};
