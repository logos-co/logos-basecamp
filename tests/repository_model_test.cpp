#include "RepositoryModel.h"

#include <QtTest/QtTest>

class RepositoryModelTest : public QObject {
    Q_OBJECT

private slots:
    void replaceAll_populates_roles()
    {
        RepositoryModel model;
        QVariantMap entry;
        entry.insert(QStringLiteral("url"), QStringLiteral("https://example.com/catalog"));
        entry.insert(QStringLiteral("displayName"), QStringLiteral("Example"));
        entry.insert(QStringLiteral("enabled"), true);
        entry.insert(QStringLiteral("isDefault"), false);
        model.replaceAll({ entry });

        QCOMPARE(model.rowCount(), 1);
        const QModelIndex idx = model.index(0);
        QCOMPARE(model.data(idx, RepositoryModel::UrlRole).toString(),
                 QStringLiteral("https://example.com/catalog"));
        QCOMPARE(model.data(idx, RepositoryModel::DisplayNameRole).toString(),
                 QStringLiteral("Example"));
        QCOMPARE(model.data(idx, RepositoryModel::EnabledRole).toBool(), true);
    }
};

QTEST_GUILESS_MAIN(RepositoryModelTest)
#include "repository_model_test.moc"
