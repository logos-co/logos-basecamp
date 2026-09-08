// srcdeps: links/SingleInstanceGuard.cpp links/LinkUrl.cpp
// libdeps: Qt6::Network
//
// The warm path's transport. Two instances, a local socket, and one URL handed
// across it.
//
// The property worth a test of its own is TRANSPORT EQUIVALENCE: a link must
// mean the same thing however it reached the shell. LinkUrl refuses control
// characters rather than stripping them, precisely so the newline-framed socket
// and a shell's argv cannot disagree about where a URL ends — but that check
// lives in the parser, and link_url_test.cpp never crosses the socket. A guard
// that trimmed its side would satisfy every parser test and still reopen the
// differential, which is why these cases go through the real QLocalServer
// rather than calling parse() with a doctored string.

#include "links/LinkUrl.h"
#include "links/SingleInstanceGuard.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class TestSingleInstanceGuard : public QObject
{
    Q_OBJECT

private slots:
    void testSecondInstanceForwardsAndDoesNotBecomePrimary();
    void testControlCharactersSurviveTheSocketAndAreRefused();
    void testDifferentUserDirsDoNotSeeEachOther();

private:
    // Each case gets its own directory, so the socket name — derived from it —
    // is unique to the case and the cases cannot collide when run in parallel.
    QTemporaryDir m_dir;
};

void TestSingleInstanceGuard::testSecondInstanceForwardsAndDoesNotBecomePrimary()
{
    QVERIFY(m_dir.isValid());
    const QString userDir = m_dir.filePath(QStringLiteral("forward"));

    SingleInstanceGuard primary;
    QCOMPARE(primary.acquire(userDir, QString()), SingleInstanceGuard::Primary);
    QVERIFY(primary.isPrimary());

    QSignalSpy received(&primary, &SingleInstanceGuard::urlReceived);

    SingleInstanceGuard secondary;
    QCOMPARE(secondary.acquire(userDir, QStringLiteral("basecamp://app/demo")),
             SingleInstanceGuard::Secondary);
    QVERIFY(!secondary.isPrimary());

    QVERIFY(received.wait(5000));
    QCOMPARE(received.size(), 1);
    QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("basecamp://app/demo"));
}

void TestSingleInstanceGuard::testControlCharactersSurviveTheSocketAndAreRefused()
{
    QVERIFY(m_dir.isValid());
    const QString userDir = m_dir.filePath(QStringLiteral("control"));

    SingleInstanceGuard primary;
    QCOMPARE(primary.acquire(userDir, QString()), SingleInstanceGuard::Primary);

    QSignalSpy received(&primary, &SingleInstanceGuard::urlReceived);

    // A tab either side. Trimming would make this parse as the clean URL it
    // surrounds, and the same bytes on the command line would be refused.
    const QString hostile = QStringLiteral("\tbasecamp://app/demo\t");
    QCOMPARE(LinkUrl::parse(hostile).kind, LinkUrl::Request::Invalid);

    SingleInstanceGuard secondary;
    QCOMPARE(secondary.acquire(userDir, hostile), SingleInstanceGuard::Secondary);

    QVERIFY(received.wait(5000));
    QCOMPARE(received.size(), 1);

    // Delivered verbatim...
    const QString delivered = received.at(0).at(0).toString();
    QCOMPARE(delivered, hostile);

    // ...and therefore refused, exactly as the argv path refuses it. Asserting
    // BOTH halves is the point: equal delivery is what makes the two verdicts
    // equal, and a test that only checked the verdict would pass against a
    // guard that stripped the tabs and a parser that had never seen them.
    QCOMPARE(LinkUrl::parse(delivered).kind, LinkUrl::Request::Invalid);
}

void TestSingleInstanceGuard::testDifferentUserDirsDoNotSeeEachOther()
{
    QVERIFY(m_dir.isValid());

    // --user-dir exists so instances can run side by side against isolated
    // data trees. Both of these are primaries; neither hears the other.
    SingleInstanceGuard first;
    QCOMPARE(first.acquire(m_dir.filePath(QStringLiteral("a")), QString()),
             SingleInstanceGuard::Primary);

    SingleInstanceGuard second;
    QCOMPARE(second.acquire(m_dir.filePath(QStringLiteral("b")),
                            QStringLiteral("basecamp://app/demo")),
             SingleInstanceGuard::Primary);

    QSignalSpy received(&first, &SingleInstanceGuard::urlReceived);
    QVERIFY(!received.wait(500));
}

QTEST_MAIN(TestSingleInstanceGuard)
#include "single_instance_guard_test.moc"
