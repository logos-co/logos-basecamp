#pragma once

#include <QSortFilterProxyModel>
#include <QString>

class AppsModel;

// Filter proxy over AppsModel. One proxy = one fully-configured view.
// All four filters are checked per-row; rows that fail any one are
// hidden. Setters fire invalidateFilter automatically.
//
// Filters:
//   typeFilter         — exact match against TypeRole (e.g. "ui_qml"). Empty = all.
//   categoryFilter     — case-insensitive exact match against CategoryRole, capitalized.
//                        "All" or empty = all categories.
//   searchText         — case-insensitive substring against NameRole. Empty = no search.
//   installStateFilter — "all" / "installed" / "notInstalled". Empty = all.
//   excludeMainUi      — drop the "main_ui" placeholder row (basecamp itself).
class AppsFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString typeFilter         READ typeFilter         WRITE setTypeFilter         NOTIFY typeFilterChanged)
    Q_PROPERTY(QString categoryFilter     READ categoryFilter     WRITE setCategoryFilter     NOTIFY categoryFilterChanged)
    Q_PROPERTY(QString searchText         READ searchText         WRITE setSearchText         NOTIFY searchTextChanged)
    Q_PROPERTY(QString installStateFilter READ installStateFilter WRITE setInstallStateFilter NOTIFY installStateFilterChanged)
    Q_PROPERTY(bool    excludeMainUi      READ excludeMainUi      WRITE setExcludeMainUi      NOTIFY excludeMainUiChanged)
    Q_PROPERTY(QStringList requiredPackages READ requiredPackages WRITE setRequiredPackages NOTIFY requiredPackagesChanged)
    Q_PROPERTY(int  installedCount       READ installedCount NOTIFY installedCountChanged)
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
    bool    excludeMainUi()      const { return m_excludeMainUi; }

    void setTypeFilter(const QString& t);
    void setCategoryFilter(const QString& c);
    void setSearchText(const QString& s);
    void setInstallStateFilter(const QString& s);
    void setExcludeMainUi(bool e);
    QStringList requiredPackages() const { return m_requiredPackages; }
    void setRequiredPackages(const QStringList& names);

    int  installedCount() const;
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
    void excludeMainUiChanged();
    void requiredPackagesChanged();
    void installedCountChanged();
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
    bool    m_excludeMainUi      = true;
    QStringList m_requiredPackages;
    QHash<QString, int> m_requiredPackagesIndex; // name → position; rebuilt on setRequiredPackages
    bool m_requiredPackagesActive = false;       // flipped true on first setRequiredPackages call
};
