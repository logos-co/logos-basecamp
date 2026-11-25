// srcdeps: utils/HostProfile.cpp
//
// Which intents attention a launch gets. "immediate" opens every module's
// requests with no click, so only a developer build takes it.

#include "utils/HostProfile.h"

#include <QTest>

using LogosBasecamp::intentsAttentionFor;

class TestHostProfile : public QObject {
    Q_OBJECT

private slots:
    void theDefaultIsBundled();
    void immediateIsForDeveloperBuildsOnly();
    void anUnknownValueFallsBackWithAWarning();
};

void TestHostProfile::theDefaultIsBundled()
{
    QString warning;
    QCOMPARE(LogosBasecamp::HostProfile().intentsAttention, QStringLiteral("bundled"));
    QCOMPARE(intentsAttentionFor(QString(), false, &warning), QStringLiteral("bundled"));
    QCOMPARE(intentsAttentionFor(QStringLiteral("interactive"), false, &warning), QStringLiteral("interactive"));
    QVERIFY(warning.isEmpty());
}

void TestHostProfile::immediateIsForDeveloperBuildsOnly()
{
    QString warning;
    QCOMPARE(intentsAttentionFor(QStringLiteral("immediate"), true, &warning), QStringLiteral("immediate"));
    QVERIFY(warning.isEmpty());
    QCOMPARE(intentsAttentionFor(QStringLiteral("immediate"), false, &warning), QStringLiteral("bundled"));
    QVERIFY2(warning.contains(QStringLiteral("developer builds")), qPrintable(warning));
}

void TestHostProfile::anUnknownValueFallsBackWithAWarning()
{
    QString warning;
    QCOMPARE(intentsAttentionFor(QStringLiteral("trusted"), true, &warning), QStringLiteral("bundled"));
    QVERIFY2(warning.contains(QStringLiteral("trusted")), qPrintable(warning));
}

QTEST_APPLESS_MAIN(TestHostProfile)
#include "host_profile_test.moc"
