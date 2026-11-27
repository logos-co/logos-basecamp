// srcdeps: ModuleCallFailure.cpp
#include <QtTest/QtTest>

#include "ModuleCallFailure.h"

// A call to package_downloader that timed out used to read as an empty
// repository list, or as "Operation failed" for a change that went through a
// minute later.
class ModuleCallFailureTest : public QObject {
    Q_OBJECT

    const QString module = QStringLiteral("package_downloader");

private slots:
    void a_timeout_says_so()
    {
        QCOMPARE(moduleCallFailure(module, "timeout", "call to 'x' timed out after 30000ms", 30 * 1000),
                 QStringLiteral("package_downloader did not answer within 30 s."));
        QCOMPARE(moduleCallFailure(module, "timeout", "", 2 * 60 * 1000),
                 QStringLiteral("package_downloader did not answer within 2 min."));
    }

    void anything_else_is_in_the_modules_words()
    {
        QCOMPARE(moduleCallFailure(module, "transport_error", "connection lost", 30 * 1000),
                 QStringLiteral("connection lost"));
        QCOMPARE(moduleCallFailure(module, "call_failed", "", 30 * 1000),
                 QStringLiteral("package_downloader did not reply."));
    }

    void a_change_that_timed_out_may_still_be_made()
    {
        QCOMPARE(moduleChangeFailure(module, "timeout", "", kRepositoryChangeMs),
                 QStringLiteral("package_downloader did not answer within 90 s. "
                                "The change may still be made; the list updates when it is."));
        QCOMPARE(moduleChangeFailure(module, "transport_error", "connection lost", kRepositoryChangeMs),
                 QStringLiteral("connection lost"));
    }

    // The downloader gives a repository's index 60 s; a change waits longer.
    void a_change_outlasts_the_index_fetch()
    {
        QVERIFY(kRepositoryChangeMs > 60 * 1000);
    }
};

QTEST_GUILESS_MAIN(ModuleCallFailureTest)
#include "module_call_failure_test.moc"
