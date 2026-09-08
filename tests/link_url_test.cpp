// srcdeps: links/LinkUrl.cpp
//
// The `basecamp://` URL parser. This is the one part of deep linking that is
// ENTIRELY attacker-controlled — anyone can put a link on a page — so it is
// also the part worth testing exhaustively, and it is deliberately free of the
// broker, the registry and any I/O so that is cheap to do.
//
// Two properties here are security properties rather than behavioural ones:
//
//   1. Control characters are REFUSED, not stripped. A URL crosses the
//      single-instance socket newline-framed; a parser that stripped what the
//      framing layer treats as a delimiter is how a parser differential starts.
//      (Status strips. That is right for their transport and wrong for ours.)
//
//   2. A payload that is not a JSON OBJECT is refused. Params reach a
//      provider's handler, and `specViolation` is a security check rather than
//      a convenience once the caller is a web page.

#include "links/LinkUrl.h"
#include "ShellIntents.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QVariantMap>

namespace {

QString encodeParams(const QVariantMap& params)
{
    const QByteArray json =
        QJsonDocument(QJsonObject::fromVariantMap(params)).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.toBase64(QByteArray::Base64UrlEncoding));
}

} // namespace

class TestLinkUrl : public QObject
{
    Q_OBJECT

private slots:
    void testBareUrlRaisesOnly();
    void testAppFormBecomesTheLaunchIntent();
    void testAppFormRejectsEmptyAndNested();
    void testIntentFormWithoutPayload();
    void testIntentFormRoundTripsTypedPayload();
    void testIntentNameMustPassTheGrammar();
    void testWrongSchemeRefused();
    void testUnknownFormRefused();
    void testControlCharactersRefusedNotStripped();
    void testPayloadMustBeAJsonObject();
    void testMalformedBase64Refused();
    void testUnknownQueryKeyRefused();
    void testRepeatedPayloadKeyRefused();
    void testOversizedUrlRefused();
    void testNonCanonicalPayloadRefused();
};

void TestLinkUrl::testBareUrlRaisesOnly()
{
    // No intent submitted. Putting a no-op through resolve, consent and
    // dispatch would buy nothing the single-instance guard does not already do.
    for (const QString& raw : { QStringLiteral("basecamp://"),
                                QStringLiteral("basecamp:///") }) {
        const auto r = LinkUrl::parse(raw);
        QVERIFY2(r.kind == LinkUrl::Request::RaiseOnly, qPrintable(raw + " -> " + r.error));
        QVERIFY(r.intent.isEmpty());
    }
}

void TestLinkUrl::testAppFormBecomesTheLaunchIntent()
{
    const auto r = LinkUrl::parse(QStringLiteral("basecamp://app/wallet_ui"));
    QCOMPARE(r.kind, LinkUrl::Request::Intent);
    QCOMPARE(r.intent, ShellIntents::kAppLaunchIntent);
    QCOMPARE(r.params.value(ShellIntents::kAppLaunchParam).toString(),
             QStringLiteral("wallet_ui"));

    // The app name is a PARAMETER, so it is not bound by the intent-name
    // grammar — which is the whole reason the name is not in the intent. A
    // hyphenated module name has to survive.
    const auto hyphen = LinkUrl::parse(QStringLiteral("basecamp://app/my-app"));
    QCOMPARE(hyphen.kind, LinkUrl::Request::Intent);
    QCOMPARE(hyphen.params.value(ShellIntents::kAppLaunchParam).toString(),
             QStringLiteral("my-app"));
}

void TestLinkUrl::testAppFormRejectsEmptyAndNested()
{
    QCOMPARE(LinkUrl::parse(QStringLiteral("basecamp://app/")).kind,
             LinkUrl::Request::Invalid);
    QCOMPARE(LinkUrl::parse(QStringLiteral("basecamp://app")).kind,
             LinkUrl::Request::Invalid);
    // A second segment would be silently discarded otherwise.
    QCOMPARE(LinkUrl::parse(QStringLiteral("basecamp://app/a/b")).kind,
             LinkUrl::Request::Invalid);
}

void TestLinkUrl::testIntentFormWithoutPayload()
{
    const auto r = LinkUrl::parse(QStringLiteral("basecamp://intent/basecamp.settings.open"));
    QCOMPARE(r.kind, LinkUrl::Request::Intent);
    QCOMPARE(r.intent, QStringLiteral("basecamp.settings.open"));
    QVERIFY(r.params.isEmpty());
}

void TestLinkUrl::testIntentFormRoundTripsTypedPayload()
{
    // The reason for base64url-of-JSON rather than query pairs: the broker
    // type-checks params against the provider's declared spec, and `?amount=12.5`
    // would arrive as the STRING "12.5" and be refused with no way to say
    // otherwise.
    QVariantMap params;
    params.insert(QStringLiteral("to"), QStringLiteral("0xabc"));
    params.insert(QStringLiteral("amount"), 12.5);
    params.insert(QStringLiteral("confirm"), true);

    const auto r = LinkUrl::parse(
        QStringLiteral("basecamp://intent/wallet.send?p=") + encodeParams(params));

    QCOMPARE(r.kind, LinkUrl::Request::Intent);
    QCOMPARE(r.intent, QStringLiteral("wallet.send"));
    QCOMPARE(r.params.value(QStringLiteral("to")).toString(), QStringLiteral("0xabc"));
    QCOMPARE(r.params.value(QStringLiteral("amount")).toDouble(), 12.5);
    QCOMPARE(r.params.value(QStringLiteral("amount")).typeId(), QMetaType::Double);
    QCOMPARE(r.params.value(QStringLiteral("confirm")).typeId(), QMetaType::Bool);
}

void TestLinkUrl::testIntentNameMustPassTheGrammar()
{
    for (const QString& bad : { QStringLiteral("Wallet.Send"),
                                QStringLiteral("wallet"),
                                QStringLiteral("a.b.c.d.e"),
                                QStringLiteral("wallet.send-now") }) {
        const auto r = LinkUrl::parse(QStringLiteral("basecamp://intent/") + bad);
        QVERIFY2(r.kind == LinkUrl::Request::Invalid, qPrintable(bad));
    }
}

void TestLinkUrl::testWrongSchemeRefused()
{
    for (const QString& raw : { QStringLiteral("https://logos.co/i/wallet.send"),
                                QStringLiteral("logos://intent/wallet.send"),
                                QStringLiteral("file:///etc/passwd") }) {
        QVERIFY2(LinkUrl::parse(raw).kind == LinkUrl::Request::Invalid, qPrintable(raw));
    }

    // QUrl lower-cases the scheme, so the RFC's case-insensitivity is already
    // handled and BASECAMP:// must still work.
    QCOMPARE(LinkUrl::parse(QStringLiteral("BASECAMP://app/wallet_ui")).kind,
             LinkUrl::Request::Intent);
}

void TestLinkUrl::testUnknownFormRefused()
{
    // Only `app` and `intent`. A new verb has to be added deliberately rather
    // than falling through to one of the existing two.
    for (const QString& raw : { QStringLiteral("basecamp://launch/wallet_ui"),
                                QStringLiteral("basecamp://install/wallet_ui"),
                                QStringLiteral("basecamp://intents/wallet.send") }) {
        QVERIFY2(LinkUrl::parse(raw).kind == LinkUrl::Request::Invalid, qPrintable(raw));
    }
}

void TestLinkUrl::testControlCharactersRefusedNotStripped()
{
    // A newline is the socket's frame delimiter. Stripping it — rather than
    // refusing the whole URL — means the framing layer and the parser disagree
    // about where one message ends, which is a parser differential.
    for (const QString& raw : { QStringLiteral("basecamp://app/wallet\nui"),
                                QStringLiteral("basecamp://app/wallet\rui"),
                                QStringLiteral("basecamp://app/wallet\tui") }) {
        const auto r = LinkUrl::parse(raw);
        QVERIFY2(r.kind == LinkUrl::Request::Invalid, qPrintable(r.intent));
    }
}

void TestLinkUrl::testPayloadMustBeAJsonObject()
{
    const auto encode = [](const QByteArray& json) {
        return QString::fromUtf8(json.toBase64(QByteArray::Base64UrlEncoding));
    };

    for (const QByteArray& json : { QByteArray("[1,2,3]"),
                                    QByteArray("\"a string\""),
                                    QByteArray("42"),
                                    QByteArray("null") }) {
        const auto r = LinkUrl::parse(
            QStringLiteral("basecamp://intent/wallet.send?p=") + encode(json));
        QVERIFY2(r.kind == LinkUrl::Request::Invalid, json.constData());
    }
}

void TestLinkUrl::testMalformedBase64Refused()
{
    for (const QString& p : { QStringLiteral("!!!not-base64!!!"),
                              QStringLiteral("YWJj+/=="),   // standard alphabet, not url
                              QStringLiteral("zzzz") }) {   // decodes, but not JSON
        const auto r = LinkUrl::parse(
            QStringLiteral("basecamp://intent/wallet.send?p=") + p);
        QVERIFY2(r.kind == LinkUrl::Request::Invalid, qPrintable(p));
    }
}

void TestLinkUrl::testUnknownQueryKeyRefused()
{
    // Ignoring it would leave someone who wrote `?params=` watching a provider
    // reject an empty payload with nothing pointing at the typo.
    const auto r = LinkUrl::parse(
        QStringLiteral("basecamp://intent/wallet.send?params=e30"));
    QCOMPARE(r.kind, LinkUrl::Request::Invalid);
}

void TestLinkUrl::testRepeatedPayloadKeyRefused()
{
    // Every key here IS `p`, so a check that only looks at key NAMES passes
    // this. QUrlQuery::queryItemValue() then answers with the first match, so
    // the shell would act on A and discard B without a word — the caller wrote
    // one payload and the provider received another.
    QVariantMap first;
    first.insert(QStringLiteral("to"), QStringLiteral("0xaaa"));
    QVariantMap second;
    second.insert(QStringLiteral("to"), QStringLiteral("0xbbb"));

    const auto r = LinkUrl::parse(QStringLiteral("basecamp://intent/wallet.send?p=")
                                  + encodeParams(first)
                                  + QStringLiteral("&p=")
                                  + encodeParams(second));
    QCOMPARE(r.kind, LinkUrl::Request::Invalid);

    // And one `p` still works — the check must not have cost the ordinary case.
    const auto ok = LinkUrl::parse(QStringLiteral("basecamp://intent/wallet.send?p=")
                                   + encodeParams(first));
    QCOMPARE(ok.kind, LinkUrl::Request::Intent);
    QCOMPARE(ok.params.value(QStringLiteral("to")).toString(), QStringLiteral("0xaaa"));
}

void TestLinkUrl::testOversizedUrlRefused()
{
    const QString huge = QStringLiteral("basecamp://intent/wallet.send?p=")
                       + QString(9000, QLatin1Char('a'));
    QCOMPARE(LinkUrl::parse(huge).kind, LinkUrl::Request::Invalid);
}

void TestLinkUrl::testNonCanonicalPayloadRefused()
{
    // The broker checks this too, right after submit. Doing it here as well is
    // not redundant: a refusal at this point names the URL in the log, where a
    // broker refusal names only a dispatch id nobody can trace back.
    QJsonObject deep;
    QJsonObject cursor;
    cursor.insert(QStringLiteral("leaf"), 1);
    for (int i = 0; i < 12; ++i) {
        QJsonObject wrap;
        wrap.insert(QStringLiteral("n"), cursor);
        cursor = wrap;
    }
    deep = cursor;

    const QString encoded = QString::fromUtf8(
        QJsonDocument(deep).toJson(QJsonDocument::Compact)
            .toBase64(QByteArray::Base64UrlEncoding));

    const auto r = LinkUrl::parse(
        QStringLiteral("basecamp://intent/wallet.send?p=") + encoded);
    QCOMPARE(r.kind, LinkUrl::Request::Invalid);
}

QTEST_MAIN(TestLinkUrl)
#include "link_url_test.moc"
