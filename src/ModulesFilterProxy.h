#pragma once

#include <QByteArray>
#include <QSortFilterProxyModel>
#include <QString>
#include <QtQml/qqml.h>

// Filter + sort proxy over ModuleInstanceModel for the Settings inspectors.
// Replaces the JS-side filter/sort logic that used to live in
// ModuleTableModel.qml, matching the AppsFilterProxy pattern the App Manager
// uses.
//
// - searchText: case-insensitive substring across name/label/statusText/
//   description/version.
// - stateFilter: "all" | "loaded" | "notLoaded" (kept as a knob even though
//   the current inspector UI doesn't expose a state tab — a follow-up may
//   restore it, and headless tests still exercise it).
// - sortRoleName: role name string ("label", "cpu", …) so QML can bind
//   LogosTable.sortRole through unchanged.
// - visibleCount / totalCount for the inspector's row-count readout.
class ModuleInstanceModel;

class ModulesFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString searchText   READ searchText   WRITE setSearchText   NOTIFY searchTextChanged)
    Q_PROPERTY(QString stateFilter  READ stateFilter  WRITE setStateFilter  NOTIFY stateFilterChanged)
    Q_PROPERTY(QString sortRoleName READ sortRoleName WRITE setSortRoleName NOTIFY sortRoleNameChanged)
    Q_PROPERTY(int     visibleCount READ visibleCount NOTIFY visibleCountChanged)
    Q_PROPERTY(int     totalCount   READ totalCount   NOTIFY totalCountChanged)
public:
    explicit ModulesFilterProxy(QObject* parent = nullptr);

    QString searchText()   const { return m_searchText; }
    QString stateFilter()  const { return m_stateFilter; }
    QString sortRoleName() const { return m_sortRoleName; }
    int     visibleCount() const { return rowCount(); }
    int     totalCount()   const;

    void setSearchText(const QString& s);
    void setStateFilter(const QString& s);
    void setSortRoleName(const QString& name);

    void setSourceModel(QAbstractItemModel* sourceModel) override;

signals:
    void searchTextChanged();
    void stateFilterChanged();
    void sortRoleNameChanged();
    void visibleCountChanged();
    void totalCountChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    // Resolves a role name (as declared by ModuleInstanceModel::roleNames())
    // back to the enum int, so consumers can drive sort/filter with the same
    // strings they use in QML delegates.
    int roleFromName(const QByteArray& name) const;

    QString m_searchText;
    QString m_stateFilter = QStringLiteral("all");
    QString m_sortRoleName = QStringLiteral("label");
};
