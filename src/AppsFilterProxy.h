#pragma once

#include <QSortFilterProxyModel>
#include <QString>
#include <QtQml/qqml.h>

class AppsModel;

// Filter proxy over AppsModel, for the welcome page's search. One proxy = one
// configured view; all filters are AND-ed per-row and setters call
// invalidateFilter automatically. A package this host can't install is left
// out unless it is installed.
class AppsFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString typeFilter         READ typeFilter         WRITE setTypeFilter         NOTIFY typeFilterChanged)
    Q_PROPERTY(QString searchText         READ searchText         WRITE setSearchText         NOTIFY searchTextChanged)
    Q_PROPERTY(bool    excludeMainUi      READ excludeMainUi      WRITE setExcludeMainUi      NOTIFY excludeMainUiChanged)
    Q_PROPERTY(int  visibleCount         READ visibleCount    NOTIFY visibleCountChanged)
public:
    explicit AppsFilterProxy(QObject* parent = nullptr);

    QString typeFilter()         const { return m_typeFilter; }
    QString searchText()         const { return m_searchText; }
    bool    excludeMainUi()      const { return m_excludeMainUi; }

    void setTypeFilter(const QString& t);
    void setSearchText(const QString& s);
    void setExcludeMainUi(bool e);

    int  visibleCount() const { return rowCount(); }

signals:
    void typeFilterChanged();
    void searchTextChanged();
    void excludeMainUiChanged();
    void visibleCountChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString m_typeFilter;
    QString m_searchText;
    bool    m_excludeMainUi      = true;
};
