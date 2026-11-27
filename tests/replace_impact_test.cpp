// What an install or upgrade stops by replacing installed packages: Basecamp
// unloads it before answering, as the old in-shell App Manager did, and the
// dialog names it. The extracted gate used to replace dependencies under
// running apps.

#include "utils/ReplaceImpact.h"

#include <QTest>

using namespace logos::replace;

namespace {

QVariantMap row(const QString& name, const QString& action = {})
{
    return {{QStringLiteral("name"), name}, {QStringLiteral("action"), action}};
}

}  // namespace

class TestReplaceImpact : public QObject {
    Q_OBJECT

private slots:
    void aPlanReplacesTheRowsItsTestSays()
    {
        const auto replaces = [](const QVariantMap& r) {
            return r.value("name") == "signer" || r.value("name") == "wallet";
        };
        QCOMPARE(replacedBy({row("wallet"), row("signer"), row("fresh"), row("signer")}, replaces),
                 (QStringList{"wallet", "signer"}));
        QVERIFY(replacedBy({row("fresh")}, replaces).isEmpty());
    }

    void replacingStopsItAndWhatRunsOnIt()
    {
        const auto running = [](const QString& n) { return n == "signer" || n == "wallet_ui"; };
        QCOMPARE(closedBy("signer", {"wallet_ui", "chat_ui"}, running),
                 (QStringList{"signer", "wallet_ui"}));
        QVERIFY(closedBy("idle", {"chat_ui"}, running).isEmpty());
    }

    // Only a row that replaces an installed release closes anything.
    void onlyAReplacingRowSaysWhatItCloses()
    {
        const auto closed = [](const QString&) { return QStringList{"wallet_ui"}; };
        const QVariantList rows = withCloses({row("a", "upgrade"), row("b", "downgrade"), row("c", "reinstall"),
                                              row("d", "install"), row("e", "installed")},
                                             closed);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(rows[i].toMap().value("closes").toStringList(), QStringList{"wallet_ui"});
        for (int i = 3; i < 5; ++i)
            QVERIFY(!rows[i].toMap().contains("closes"));
        const QVariantList none = withCloses({row("a", "upgrade")}, [](const QString&) { return QStringList{}; });
        QVERIFY(!none[0].toMap().contains("closes"));
    }
};

QTEST_GUILESS_MAIN(TestReplaceImpact)
#include "replace_impact_test.moc"
