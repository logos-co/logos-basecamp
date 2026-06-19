#pragma once

#include <QSortFilterProxyModel>
#include <QString>
#include <QtQml/qqml.h>

class AppsModel;

// Filter proxy over AppsModel. One proxy = one configured view; all filters
// are AND-ed per-row and setters call invalidateFilter automatically.
class AppsFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString typeFilter         READ typeFilter         WRITE setTypeFilter         NOTIFY typeFilterChanged)
    Q_PROPERTY(QString categoryFilter     READ categoryFilter     WRITE setCategoryFilter     NOTIFY categoryFilterChanged)
    Q_PROPERTY(QString searchText         READ searchText         WRITE setSearchText         NOTIFY searchTextChanged)
    Q_PROPERTY(QString installStateFilter READ installStateFilter WRITE setInstallStateFilter NOTIFY installStateFilterChanged)
    Q_PROPERTY(QString repositoryUrlFilter READ repositoryUrlFilter WRITE setRepositoryUrlFilter NOTIFY repositoryUrlFilterChanged)
    Q_PROPERTY(bool    excludeMainUi      READ excludeMainUi      WRITE setExcludeMainUi      NOTIFY excludeMainUiChanged)
    Q_PROPERTY(QStringList requiredPackages READ requiredPackages NOTIFY requiredPackagesChanged)
    Q_PROPERTY(int  installedCount       READ installedCount NOTIFY installedCountChanged)
    Q_PROPERTY(int  installFreshCount      READ installFreshCount      NOTIFY breakdownChanged)
    Q_PROPERTY(int  upgradeCount           READ upgradeCount           NOTIFY breakdownChanged)
    Q_PROPERTY(int  reinstallCount         READ reinstallCount         NOTIFY breakdownChanged)
    Q_PROPERTY(int  alreadyInstalledCount  READ alreadyInstalledCount  NOTIFY breakdownChanged)
    Q_PROPERTY(int  errorCount             READ errorCount             NOTIFY breakdownChanged)
    Q_PROPERTY(bool hasResolutionErrors  READ hasResolutionErrors NOTIFY hasResolutionErrorsChanged)
    Q_PROPERTY(int  visibleCount         READ visibleCount    NOTIFY visibleCountChanged)
    Q_PROPERTY(qlonglong totalDownloadBytes READ totalDownloadBytes NOTIFY totalDownloadBytesChanged)
    Q_PROPERTY(QStringList categories    READ categories      NOTIFY categoriesChanged)
public:
    explicit AppsFilterProxy(QObject* parent = nullptr);

    QString typeFilter()         const { return m_typeFilter; }
    QString categoryFilter()     const { return m_categoryFilter; }
    QString searchText()         const { return m_searchText; }
    QString installStateFilter() const { return m_installStateFilter; }
    QString repositoryUrlFilter() const { return m_repositoryUrlFilter; }
    bool    excludeMainUi()      const { return m_excludeMainUi; }

    void setTypeFilter(const QString& t);
    void setCategoryFilter(const QString& c);
    void setSearchText(const QString& s);
    void setInstallStateFilter(const QString& s);
    void setRepositoryUrlFilter(const QString& url);
    void setExcludeMainUi(bool e);
    QStringList requiredPackages() const;
    Q_INVOKABLE void setRequiredPackages(const QVariantList& entries);

    int  installedCount() const;
    int  installFreshCount() const;
    int  upgradeCount() const;
    int  reinstallCount() const;
    int  alreadyInstalledCount() const;
    int  errorCount() const;
    bool hasResolutionErrors() const;
    int  visibleCount() const { return rowCount(); }
    qlonglong totalDownloadBytes() const;
    QStringList categories() const;

    void setSourceModel(QAbstractItemModel* sourceModel) override;

signals:
    void typeFilterChanged();
    void categoryFilterChanged();
    void searchTextChanged();
    void installStateFilterChanged();
    void repositoryUrlFilterChanged();
    void excludeMainUiChanged();
    void requiredPackagesChanged();
    void installedCountChanged();
    void breakdownChanged();
    void hasResolutionErrorsChanged();
    void visibleCountChanged();
    void totalDownloadBytesChanged();
    void categoriesChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    static QString capitalizeFirst(const QString& s);

    QString m_typeFilter;
    QString m_categoryFilter;
    QString m_searchText;
    QString m_installStateFilter = QStringLiteral("all");
    QString m_repositoryUrlFilter;
    bool    m_excludeMainUi      = true;
    QHash<QString, QString> m_requiredPackagesByName;
    QHash<QString, int>     m_requiredPackagesOrder;
    bool m_requiredPackagesActive = false;
};
