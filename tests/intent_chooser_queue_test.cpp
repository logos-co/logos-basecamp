// srcdeps: IntentChooserQueue.cpp
//
// One chooser on screen at a time, from either broker: a second never repoints
// the one the person is deciding on, and comes up once that one has closed.

#include "IntentChooserQueue.h"

#include <QSignalSpy>
#include <QTest>

namespace {

IntentChooserQueue::Chooser chooser(const char* id)
{
    return {QLatin1String(id), QStringLiteral("wallet.send"), QStringLiteral("chat_ui"), {}};
}

QStringList ids(const QSignalSpy& spy)
{
    QStringList out;
    for (const QList<QVariant>& args : spy) out << args.value(0).toString();
    return out;
}

} // namespace

class TestIntentChooserQueue : public QObject {
    Q_OBJECT

private slots:
    void theFirstIsShownAtOnce();
    void aSecondWaitsUntilTheFirstHasClosed();
    void theNextIsTheOldestWaiting();
    void aWithdrawnWaitingOneIsNeverShown();
    void aWithdrawnShownOneIsClosedAndStillHoldsTheSlotUntilItHas();
    void aCloseForAnotherIdChangesNothing();
    void theSameRequestIsQueuedOnce();
};

void TestIntentChooserQueue::theFirstIsShownAtOnce()
{
    IntentChooserQueue q;
    QSignalSpy shown(&q, &IntentChooserQueue::show);
    q.request(chooser("a"));
    QCOMPARE(ids(shown), QStringList{QStringLiteral("a")});
    QCOMPARE(q.onScreen(), QStringLiteral("a"));
}

// The consent swap this exists to stop: request B arriving while A is up.
void TestIntentChooserQueue::aSecondWaitsUntilTheFirstHasClosed()
{
    IntentChooserQueue q;
    QSignalSpy shown(&q, &IntentChooserQueue::show);
    q.request(chooser("a"));
    q.request(chooser("b"));
    QCOMPARE(ids(shown), QStringList{QStringLiteral("a")});
    QCOMPARE(q.waiting(), 1);
    q.closed(QStringLiteral("a"));
    QCOMPARE(ids(shown), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
    QCOMPARE(q.onScreen(), QStringLiteral("b"));
}

void TestIntentChooserQueue::theNextIsTheOldestWaiting()
{
    IntentChooserQueue q;
    QSignalSpy shown(&q, &IntentChooserQueue::show);
    for (const char* id : {"a", "b", "c"}) q.request(chooser(id));
    q.closed(QStringLiteral("a"));
    q.closed(QStringLiteral("b"));
    QCOMPARE(ids(shown), (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));
}

void TestIntentChooserQueue::aWithdrawnWaitingOneIsNeverShown()
{
    IntentChooserQueue q;
    QSignalSpy shown(&q, &IntentChooserQueue::show);
    QSignalSpy closing(&q, &IntentChooserQueue::close);
    q.request(chooser("a"));
    q.request(chooser("b"));
    q.withdraw(QStringLiteral("b"));
    QCOMPARE(closing.size(), 0);
    q.closed(QStringLiteral("a"));
    QCOMPARE(ids(shown), QStringList{QStringLiteral("a")});
    QCOMPARE(q.onScreen(), QString());
}

// Taken down by its broker: the dialog closes it, and the next waits for that.
void TestIntentChooserQueue::aWithdrawnShownOneIsClosedAndStillHoldsTheSlotUntilItHas()
{
    IntentChooserQueue q;
    QSignalSpy shown(&q, &IntentChooserQueue::show);
    QSignalSpy closing(&q, &IntentChooserQueue::close);
    q.request(chooser("a"));
    q.request(chooser("b"));
    q.withdraw(QStringLiteral("a"));
    QCOMPARE(ids(closing), QStringList{QStringLiteral("a")});
    QCOMPARE(ids(shown), QStringList{QStringLiteral("a")});
    q.closed(QStringLiteral("a"));
    QCOMPARE(ids(shown), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
}

void TestIntentChooserQueue::aCloseForAnotherIdChangesNothing()
{
    IntentChooserQueue q;
    QSignalSpy shown(&q, &IntentChooserQueue::show);
    q.request(chooser("a"));
    q.request(chooser("b"));
    q.closed(QStringLiteral("b"));
    QCOMPARE(q.onScreen(), QStringLiteral("a"));
    QCOMPARE(shown.size(), 1);
}

void TestIntentChooserQueue::theSameRequestIsQueuedOnce()
{
    IntentChooserQueue q;
    QSignalSpy shown(&q, &IntentChooserQueue::show);
    q.request(chooser("a"));
    q.request(chooser("a"));
    q.request(chooser("b"));
    q.request(chooser("b"));
    QCOMPARE(q.waiting(), 1);
    q.closed(QStringLiteral("a"));
    q.closed(QStringLiteral("b"));
    QCOMPARE(ids(shown), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
}

QTEST_MAIN(TestIntentChooserQueue)
#include "intent_chooser_queue_test.moc"
