// What stops a confirmation going ahead on what it wasn't planned over: a
// read of the installed packages that failed (FreshReads), a plan approved
// for later whose packages or their dependents changed since (PlanBasis),
// and which app is on screen (isFrontmost). From basecamp#578's review.

#include "utils/FreshReads.h"
#include "utils/Frontmost.h"
#include "utils/PlanBasis.h"

#include <QTest>

using namespace logos;

class TestPlanGuards : public QObject {
    Q_OBJECT

private slots:
    // A read that failed is no answer: what waited for it plans nothing. It
    // used to go on with what was known before.
    void aFailedReadIsNoAnswer()
    {
        FreshReads reads;
        QList<bool> answers;
        int generation = 0;
        reads.afterNext([&] { generation = reads.begin(); return true; },
                        [&](bool read) { answers << read; });
        QVERIFY(answers.isEmpty());
        reads.done(generation, false);
        QCOMPARE(answers, QList<bool>{false});
    }

    void noReadThatCanStartIsNoAnswer()
    {
        FreshReads reads;
        QList<bool> answers;
        reads.afterNext([] { return false; }, [&](bool read) { answers << read; });
        QCOMPARE(answers, QList<bool>{false});
    }

    // Only a read started after the request answers it; a newer one wins.
    void onlyANewerReadAnswers()
    {
        FreshReads reads;
        const int before = reads.begin();
        QList<bool> answers;
        int mine = 0;
        reads.afterNext([&] { mine = reads.begin(); return true; },
                        [&](bool read) { answers << read; });
        reads.done(before, true);
        QVERIFY(answers.isEmpty());
        const int newer = reads.begin();
        reads.done(mine, true);
        QVERIFY(answers.isEmpty());
        reads.done(newer, true);
        QCOMPARE(answers, QList<bool>{true});
    }

    void aPlanGoesAheadOnlyOverWhatItWasPlannedOn()
    {
        planbasis::Basis basis;
        basis.releases = {{"app", ""}, {"lib", "1.0\x01h1"}};
        basis.dependents = {{"lib", {"other_app"}}};
        QHash<QString, QString> releases = basis.releases;
        QHash<QString, QStringList> dependents{{"lib", {"other_app"}}};
        const auto release = [&](const QString& n) { return releases.value(n); };
        const auto dependentsOf = [&](const QString& n) { return dependents.value(n); };
        QCOMPARE(planbasis::changed(basis, release, dependentsOf), QString());

        releases["app"] = "0.9\x01h0";
        QCOMPARE(planbasis::changed(basis, release, dependentsOf), QStringLiteral("app"));
        releases["app"] = "";

        // Something installed since that depends on what the plan replaces.
        dependents["lib"] << "new_app";
        QCOMPARE(planbasis::changed(basis, release, dependentsOf), QStringLiteral("lib"));
        // One gone is no news to the person.
        dependents["lib"] = {};
        QCOMPARE(planbasis::changed(basis, release, dependentsOf), QString());
    }

    // A section app is on screen when its section is, whatever workspace app
    // was shown last. It used to need to be that app too, so App Manager's
    // section never counted as App Manager being on screen.
    void aSectionAppIsFrontmostWhenItsSectionIs()
    {
        const int workspace = 0, apps = 1;
        QVERIFY(isFrontmost("app_manager_ui", apps, apps, workspace, "wallet_ui"));
        QVERIFY(isFrontmost("app_manager_ui", apps, apps, workspace, ""));
        QVERIFY(!isFrontmost("app_manager_ui", apps, workspace, workspace, "app_manager_ui"));
        QVERIFY(isFrontmost("wallet_ui", -1, workspace, workspace, "wallet_ui"));
        QVERIFY(!isFrontmost("wallet_ui", -1, apps, workspace, "wallet_ui"));
        QVERIFY(!isFrontmost("wallet_ui", -1, workspace, workspace, "chat_ui"));
        QVERIFY(!isFrontmost("", -1, workspace, workspace, ""));
    }
};

QTEST_GUILESS_MAIN(TestPlanGuards)
#include "plan_guards_test.moc"
