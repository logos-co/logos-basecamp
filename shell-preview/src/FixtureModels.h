#pragma once

#include "BasecampModelRoles.h"

#include <QAbstractListModel>
#include <QJsonArray>
#include <QVariantMap>

// Generic list model over fixture rows, keyed by the shared role enums.
//
// Roles come from app/interfaces/BasecampModelRoles.h, which both the real
// models and the shell already share — so the QML binds identically here.
class FixtureListModel : public QAbstractListModel {
    Q_OBJECT
public:
    FixtureListModel(QHash<int, QByteArray> roles, QObject* parent = nullptr);

    void setRows(const QVariantList& rows);

    int      rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return m_roles; }

private:
    QHash<int, QByteArray> m_roles;
    QVariantList           m_rows;   // each a QVariantMap keyed by role name
};

// Role tables matching the real models.
QHash<int, QByteArray> appsRoleNames();
QHash<int, QByteArray> moduleInstanceRoleNames();
