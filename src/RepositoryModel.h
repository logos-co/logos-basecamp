#pragma once

#include <QAbstractListModel>
#include <QVariantList>

class RepositoryModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        UrlRole = Qt::UserRole + 1,
        DisplayNameRole,
        NameRole,
        DescriptionRole,
        EnabledRole,
        IsDefaultRole,
        ResolveErrorRole,
    };
    Q_ENUM(Roles)

    explicit RepositoryModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replaceAll(const QVariantList& repositories);
    int rowOfUrl(const QString& url) const;

private:
    struct Row {
        QString url;
        QString displayName;
        QString name;
        QString description;
        bool enabled = true;
        bool isDefault = false;
        QString resolveError;
    };

    QList<Row> m_rows;
    QHash<QString, int> m_indexByUrl;
};
