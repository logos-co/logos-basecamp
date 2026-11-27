// srcdeps: ConfirmationQueue.cpp
//
// One approved package change at a time, whichever app asks: a confirmation
// waits while another is on screen, and while the change approved last is
// still being made.

#include "ConfirmationQueue.h"

#include <QTest>

namespace {

const QString kInstall = QStringLiteral("basecamp.packages.confirm_install");
const QString kUninstall = QStringLiteral("basecamp.packages.confirm_uninstall");
const QString kAppManager = QStringLiteral("app_manager_ui");
const QString kPackageManager = QStringLiteral("package_manager_ui");

ConfirmationQueue::Request install(const char* id, const QString& requester, const QString& name)
{
    return {QLatin1String(id), kInstall, {{QStringLiteral("name"), name}}, requester};
}

// An install whose app holds an approval until its turn.
ConfirmationQueue::Request canWait(const char* id, const QString& requester, const QString& name)
{
    return {QLatin1String(id), kInstall, {{QStringLiteral("name"), name}, {QStringLiteral("waits"), true}},
            requester};
}

// Handing back the ticket it was approved with for later.
ConfirmationQueue::Request goingAhead(const char* id, const QString& requester, const QString& name)
{
    return {QLatin1String(id), kInstall, {{QStringLiteral("name"), name}, {QStringLiteral("ticket"), "t1"}},
            requester};
}

QVariantMap plan(const QString& name, const QString& version, const QString& rootHash = {})
{
    return {{QStringLiteral("plan"),
             QVariantList{QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("version"), version},
                                      {QStringLiteral("rootHash"), rootHash}}}}};
}

QVariantMap installedOne(const QString& name, const QString& version, const QString& rootHash = {})
{
    return {{QStringLiteral("name"), name}, {QStringLiteral("version"), version},
            {QStringLiteral("hashes"), QVariantMap{{QStringLiteral("root"), rootHash}}}};
}

QVariantList installed(const QString& name, const QString& version, const QString& rootHash = {})
{
    return {installedOne(name, version, rootHash)};
}

struct Shown {
    QStringList ids;
    ConfirmationQueue::Begin begin()
    {
        return [this](const ConfirmationQueue::Request& r) {
            ids << r.id;
            return true;
        };
    }
};

}  // namespace

class TestConfirmationQueue : public QObject {
    Q_OBJECT

private slots:
    void oneDialogAtATime()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.submit(install("b", kPackageManager, "chat"));
        QCOMPARE(shown.ids, QStringList{"a"});
        QCOMPARE(q.waiting(), 1);
        q.answered("a", false, {});
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
        QVERIFY(q.changing().isEmpty());
    }

    void anApprovedChangeHoldsTheNextUntilItIsOnDisk()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        QCOMPARE(q.changing(), kAppManager);
        q.submit(install("b", kPackageManager, "chat"));
        QCOMPARE(shown.ids, QStringList{"a"});
        q.installedChanged(installed("wallet", "0.9.0"));
        QCOMPARE(shown.ids, QStringList{"a"});
        q.installedChanged(installed("wallet", "1.0.0"));
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
        QVERIFY(q.changing().isEmpty());
    }

    // An installer app asks only once its last change has ended.
    void theAppAskingAgainEndsItsChange()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(install("b", kAppManager, "chat"));
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
    }

    // Asked about while its own app's change is made: shown at once, and
    // approved for later, which starts no change and ends none.
    void anInstallThatCanWaitIsShownWhileAChangeIsMade()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(canWait("b", kAppManager, "chat"));
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
        QCOMPARE(q.changing(), kAppManager);
        q.answered("b", true, QVariantMap{{QStringLiteral("ticket"), QStringLiteral("t1")}});
        QCOMPARE(q.changing(), kAppManager);
        q.installedChanged(installed("wallet", "1.0.0"));
        QVERIFY(q.changing().isEmpty());
    }

    // Handing its ticket back, it goes ahead once the change before it is made.
    void aTicketHandedBackWaitsForTheChange()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kPackageManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(goingAhead("b", kAppManager, "chat"));
        QCOMPARE(shown.ids, QStringList{"a"});
        q.installedChanged(installed("wallet", "1.0.0"));
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
    }

    // Only an install or upgrade that says it can wait: an uninstall waits.
    void onlyAnInstallThatCanWaitIsShownWhileAChangeIsMade()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(install("b", kPackageManager, "chat"));
        q.submit({QStringLiteral("c"), kUninstall,
                  {{QStringLiteral("names"), QStringList{"old"}}, {QStringLiteral("waits"), true}},
                  kPackageManager});
        QCOMPARE(shown.ids, QStringList{"a"});
        QCOMPARE(q.waiting(), 2);
    }

    // It never takes the place of a dialog on screen, but it goes ahead of one
    // the change holds.
    void oneThatCanWaitWaitsForTheDialogOnScreenOnly()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.submit(canWait("b", kAppManager, "chat"));
        QCOMPARE(shown.ids, QStringList{"a"});
        q.answered("a", true, plan("wallet", "1.0.0"));
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
        q.answered("b", true, QVariantMap{{QStringLiteral("ticket"), QStringLiteral("t1")}});
        q.submit(install("c", kPackageManager, "notes"));
        q.submit(canWait("d", kPackageManager, "maps"));
        QCOMPARE(shown.ids, (QStringList{"a", "b", "d"}));
        QCOMPARE(q.waiting(), 1);
    }

    void anUninstallIsMadeOnceItIsGone()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit({QStringLiteral("a"), kUninstall,
                  {{QStringLiteral("names"), QStringList{QStringLiteral("wallet")}}}, kAppManager});
        q.answered("a", true, {});
        q.submit(install("b", kPackageManager, "chat"));
        q.installedChanged(installed("wallet", "1.0.0"));
        QCOMPARE(shown.ids, QStringList{"a"});
        q.installedChanged({});
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
    }

    // An app's removal takes what only it needed: the change is everything
    // the answer names.
    void anUninstallIsMadeOnceEverythingApprovedIsGone()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit({QStringLiteral("a"), kUninstall,
                  {{QStringLiteral("names"), QStringList{QStringLiteral("wallet")}}}, kAppManager});
        q.answered("a", true, QVariantMap{{QStringLiteral("names"),
                                           QStringList{QStringLiteral("wallet"), QStringLiteral("wallet_module")}}});
        q.submit(install("b", kPackageManager, "chat"));
        q.installedChanged(installed("wallet_module", "1.0.0"));
        QCOMPARE(shown.ids, QStringList{"a"});
        q.installedChanged({});
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
    }

    // A file the gate approved by its identity: the package the request names.
    void aLocalFileIsMadeOnceItsPackageIsThere()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit({QStringLiteral("a"), kInstall,
                  {{QStringLiteral("name"), QStringLiteral("wallet")}, {QStringLiteral("version"), QStringLiteral("2.0.0")}},
                  kPackageManager});
        q.answered("a", true, QVariantMap{{QStringLiteral("file"), QVariantMap{}}});
        QCOMPARE(q.changing(), kPackageManager);
        q.installedChanged(installed("wallet", "2.0.0"));
        QVERIFY(q.changing().isEmpty());
    }

    void aChangeThatNeverShowsTimesOut()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.setChangeTimeout(20);
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(install("b", kPackageManager, "chat"));
        QCOMPARE(shown.ids, QStringList{"a"});
        QTRY_COMPARE_WITH_TIMEOUT(shown.ids, (QStringList{"a", "b"}), 1000);
    }

    // A download as long as it takes: a package of the change on its way
    // holds the next past the timeout. The next used to show ten minutes in,
    // the download still going.
    void aChangeOnItsWayHoldsTheNext()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.setChangeTimeout(1000);
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(install("b", kPackageManager, "chat"));
        for (int i = 0; i < 5; ++i) {
            QTest::qWait(300);
            q.progressed(QStringLiteral("wallet"));
        }
        QCOMPARE(shown.ids, QStringList{"a"});
        QTRY_COMPARE_WITH_TIMEOUT(shown.ids, (QStringList{"a", "b"}), 5000);
    }

    void aPackageOutsideTheChangeHoldsNothing()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.setChangeTimeout(1000);
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(install("b", kPackageManager, "chat"));
        for (int i = 0; i < 10 && shown.ids.size() == 1; ++i) {
            QTest::qWait(300);
            q.progressed(QStringLiteral("chat"));
        }
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
    }

    // The app making it says that change ended (a failed install too), by
    // the id of the request that approved it. A failed change used to hold
    // every other confirmation until the timeout.
    void theAppEndingItsChangeEndsIt()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(install("b", kPackageManager, "chat"));
        q.changeEnded(QStringLiteral("b"));
        q.changeEnded({});
        QCOMPARE(shown.ids, QStringList{"a"});
        q.changeEnded(QStringLiteral("a"));
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
        QVERIFY(q.changing().isEmpty());
    }

    // Another request of the same app ending is not its change ending: App
    // Manager cancelling an install that waited its turn, while its first
    // still downloads, used to let Package Manager's change start beside it.
    void anotherRequestOfTheSameAppEndingHoldsTheChange()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(canWait("b", kAppManager, "chat"));
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
        q.answered("b", false, {});
        q.changeEnded(QStringLiteral("b"));
        q.submit(install("c", kPackageManager, "notes"));
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
        QCOMPARE(q.changing(), kAppManager);
        q.changeEnded(QStringLiteral("a"));
        QCOMPARE(shown.ids, (QStringList{"a", "b", "c"}));
    }

    // The app that asked closed or stopped: nothing is making its change.
    void theAppGoingEndsItsChange()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(install("b", kPackageManager, "chat"));
        q.requesterGone(kPackageManager);
        q.requesterGone({});
        QCOMPARE(shown.ids, QStringList{"a"});
        q.requesterGone(kAppManager);
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
    }

    // A reinstall keeps its version: it is made once its own release is on
    // disk, not by the copy it replaces.
    void aReinstallIsMadeOnceItsOwnReleaseIsThere()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0", "new"));
        q.submit(install("b", kPackageManager, "chat"));
        q.installedChanged(installed("wallet", "1.0.0", "old"));
        QCOMPARE(shown.ids, QStringList{"a"});
        q.installedChanged(installed("wallet", "1.0.0", "new"));
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
    }

    // An optional package still installing is part of the change: it used to
    // end once the required ones were on disk. One that fails is left out,
    // and its app says the change ended.
    void anOptionalPackageStillInstallingHoldsTheChange()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        QVariantMap approved = plan("wallet", "1.0.0");
        QVariantList rows = approved.value(QStringLiteral("plan")).toList();
        rows << QVariantMap{{QStringLiteral("name"), QStringLiteral("extra")},
                            {QStringLiteral("version"), QStringLiteral("1.0.0")},
                            {QStringLiteral("optional"), true}};
        approved[QStringLiteral("plan")] = rows;
        q.answered("a", true, approved);
        q.submit(install("b", kPackageManager, "chat"));
        q.installedChanged(installed("wallet", "1.0.0"));
        QCOMPARE(shown.ids, QStringList{"a"});
        q.installedChanged({installedOne("wallet", "1.0.0"), installedOne("extra", "1.0.0")});
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));

        ConfirmationQueue failed;
        Shown shownAfterFailure;
        failed.setBegin(shownAfterFailure.begin());
        failed.submit(install("a", kAppManager, "wallet"));
        failed.answered("a", true, approved);
        failed.submit(install("b", kPackageManager, "chat"));
        failed.installedChanged(installed("wallet", "1.0.0"));
        failed.changeEnded(QStringLiteral("a"));
        QCOMPARE(shownAfterFailure.ids, (QStringList{"a", "b"}));
    }

    // Files installed or removed, whichever package: the change is moving.
    void anyPackageInstalledHoldsTheChange()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.setChangeTimeout(1000);
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(install("b", kPackageManager, "chat"));
        for (int i = 0; i < 5; ++i) {
            QTest::qWait(300);
            q.activity();
        }
        QCOMPARE(shown.ids, QStringList{"a"});
        QTRY_COMPARE_WITH_TIMEOUT(shown.ids, (QStringList{"a", "b"}), 5000);
    }

    void aWithdrawnRequestLeavesTheQueue()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.submit(install("b", kPackageManager, "chat"));
        q.submit(install("c", kPackageManager, "notes"));
        q.withdrawn("b");
        QCOMPARE(q.waiting(), 1);
        q.withdrawn("a");
        QCOMPARE(shown.ids, (QStringList{"a", "c"}));
    }

    // Refused, or answered as it was shown: the next one at once.
    void oneAnsweredAtOnceGoesOnToTheNext()
    {
        ConfirmationQueue q;
        QStringList shown;
        q.setBegin([&](const ConfirmationQueue::Request& r) {
            shown << r.id;
            if (r.id == QLatin1String("a")) {
                q.answered(r.id, false, {});
                return true;
            }
            return r.id != QLatin1String("b");
        });
        q.submit(install("x", kAppManager, "other"));
        q.answered("x", false, {});
        QCOMPARE(shown, QStringList{"x"});
        q.submit(install("y", kAppManager, "hold"));
        q.submit(install("a", kAppManager, "wallet"));
        q.submit(install("b", kAppManager, "chat"));
        q.submit(install("c", kAppManager, "notes"));
        q.answered("y", false, {});
        QCOMPARE(shown, (QStringList{"x", "y", "a", "b", "c"}));
        QCOMPARE(q.onScreen(), QStringLiteral("c"));
    }
};

QTEST_GUILESS_MAIN(TestConfirmationQueue)
#include "confirmation_queue_test.moc"
