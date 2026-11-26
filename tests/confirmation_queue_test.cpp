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

QVariantMap plan(const QString& name, const QString& version)
{
    return {{QStringLiteral("plan"),
             QVariantList{QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("version"), version}}}}};
}

QVariantList installed(const QString& name, const QString& version)
{
    return {QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("version"), version}}};
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

    // Its app stopped: nothing is making the change any more.
    void theAppStoppingEndsItsChange()
    {
        ConfirmationQueue q;
        Shown shown;
        q.setBegin(shown.begin());
        q.submit(install("a", kAppManager, "wallet"));
        q.answered("a", true, plan("wallet", "1.0.0"));
        q.submit(install("b", kPackageManager, "chat"));
        q.requesterStopped(kPackageManager);
        q.requesterStopped({});
        QCOMPARE(shown.ids, QStringList{"a"});
        q.requesterStopped(kAppManager);
        QCOMPARE(shown.ids, (QStringList{"a", "b"}));
        QVERIFY(q.changing().isEmpty());
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
