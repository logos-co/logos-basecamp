#include <QtTest/QtTest>

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

// The App Manager's catalog read (#571). A bare invokeRemoteMethodAsync
// handed a timeout over as an empty catalog and cleared every repository;
// replies to older reads could replace newer ones; and a cold catalog takes
// longer than the default 20 s deadline.
class CatalogFetchTest : public QObject {
    Q_OBJECT

    // tryFetchCatalog's body, without comments.
    static QString tryFetchCatalog()
    {
        const QString app = QFileInfo(QStringLiteral(__FILE__)).absolutePath() + QStringLiteral("/../app");
        QFile file(app + QStringLiteral("/PackageCoordinator.cpp"));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
        QString text = QString::fromUtf8(file.readAll());
        text.remove(QRegularExpression(QStringLiteral(R"(/\*.*?\*/)"),
                                       QRegularExpression::DotMatchesEverythingOption));
        text.remove(QRegularExpression(QStringLiteral(R"(//[^\n]*)")));
        const int from = text.indexOf(QStringLiteral("void PackageCoordinator::tryFetchCatalog("));
        if (from < 0) return QString();
        const int to = text.indexOf(QStringLiteral("\nvoid PackageCoordinator::"), from + 1);
        return text.mid(from, to < 0 ? -1 : to - from);
    }

private slots:
    void the_catalog_is_read_with_its_error_and_a_deadline_of_its_own()
    {
        const QString body = tryFetchCatalog();
        QVERIFY(!body.isEmpty());
        QVERIFY(body.contains(QStringLiteral("getCatalogAsyncResult(")));
        QVERIFY(!body.contains(QStringLiteral("invokeRemoteMethod")));
        QVERIFY(body.contains(QStringLiteral("Timeout(kCatalogIpcDeadlineMs)")));

        const QRegularExpression deadline(QStringLiteral(
            R"re(kCatalogIpcDeadlineMs\s*=\s*(\d+)\s*\*\s*60\s*\*\s*1000)re"));
        const QRegularExpressionMatch m = deadline.match(body);
        QVERIFY(m.hasMatch());
        // #571 measured one to two minutes for a cold catalog.
        QVERIFY(m.captured(1).toInt() >= 2);
    }

    void an_older_reply_is_dropped_and_a_failed_one_loads_nothing()
    {
        const QString body = tryFetchCatalog();
        QVERIFY(body.contains(QStringLiteral("generation != self->m_catalogGeneration")));
        QVERIFY(body.contains(QStringLiteral("catalogFetchFinished(r.ok())")));
        QVERIFY(!body.contains(QStringLiteral("catalogFetchFinished()")));
    }
};

QTEST_GUILESS_MAIN(CatalogFetchTest)
#include "catalog_fetch_test.moc"
