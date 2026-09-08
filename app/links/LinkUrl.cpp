#include "LinkUrl.h"

#include "LogosIntent.h"
#include "ShellIntents.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrlQuery>

namespace {

// A URL is carried over a newline-terminated socket between instances, and it
// arrives from argv where a shell may have mangled it. Anything with an
// embedded control character is REFUSED rather than stripped.
//
// Status strips whitespace and newlines at this point. That is right for their
// transport and wrong for ours: their URL names an in-app destination, ours
// carries a payload the broker will act on, and silently rewriting an
// attacker-supplied string is how a parser differential starts — the framing
// layer sees one thing, the parser another.
bool hasControlCharacters(const QString& s)
{
    for (const QChar c : s)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return true;
    return false;
}

// Bounded well below the payload rules' own 64 KB-per-string limit. A URL is
// typed, pasted or embedded in a page; anything approaching this is not a link.
constexpr int kMaxUrlChars = 8192;

// An app name becomes a plain string parameter, so it is not bound by the
// intent-name grammar — that asymmetry is exactly why the name is a parameter
// and not part of the intent. It still needs a bound.
constexpr int kMaxAppNameChars = 128;

LinkUrl::Request invalid(const QString& why)
{
    LinkUrl::Request r;
    r.kind = LinkUrl::Request::Invalid;
    r.error = why;
    return r;
}

} // namespace

namespace LinkUrl {

QString scheme()
{
    return QStringLiteral("basecamp");
}

Request parse(const QString& rawUrl)
{
    if (rawUrl.isEmpty())
        return invalid(QStringLiteral("empty URL"));
    if (rawUrl.size() > kMaxUrlChars)
        return invalid(QStringLiteral("URL longer than %1 characters").arg(kMaxUrlChars));
    if (hasControlCharacters(rawUrl))
        return invalid(QStringLiteral("URL contains control characters"));

    // StrictMode, never fromUserInput(): the latter guesses at malformed input
    // and would repair some of what this exists to reject.
    const QUrl url(rawUrl, QUrl::StrictMode);
    if (!url.isValid())
        return invalid(QStringLiteral("not a valid URL: %1").arg(url.errorString()));
    return parse(url);
}

Request parse(const QUrl& url)
{
    if (!url.isValid())
        return invalid(QStringLiteral("not a valid URL"));
    if (hasControlCharacters(url.toString()))
        return invalid(QStringLiteral("URL contains control characters"));

    // QUrl lower-cases the scheme, so this comparison is already
    // case-insensitive in the way RFC 3986 requires.
    if (url.scheme() != scheme())
        return invalid(QStringLiteral("scheme is '%1', not '%2'")
                           .arg(url.scheme(), scheme()));

    if (url.hasFragment())
        return invalid(QStringLiteral("fragments are not accepted"));
    if (!url.userInfo().isEmpty())
        return invalid(QStringLiteral("user info is not accepted"));

    const QString host = url.host();
    const QString path = url.path();

    // basecamp:// — raise the window and nothing else. No intent is submitted:
    // putting a no-op through resolve, consent and dispatch would buy nothing,
    // and the single-instance guard already raises on a second launch.
    if (host.isEmpty() && (path.isEmpty() || path == QStringLiteral("/")))
        return { Request::RaiseOnly, {}, {}, {} };

    // The first path segment is QUrl's `host` for an authority-form URL:
    // "basecamp://app/wallet_ui" parses as host="app", path="/wallet_ui".
    const QString tail = path.startsWith(QLatin1Char('/')) ? path.mid(1) : path;

    if (host == QStringLiteral("app")) {
        if (tail.isEmpty())
            return invalid(QStringLiteral("basecamp://app/ needs an app name"));
        if (tail.contains(QLatin1Char('/')))
            return invalid(QStringLiteral("app name must be a single path segment"));
        if (tail.size() > kMaxAppNameChars)
            return invalid(QStringLiteral("app name longer than %1 characters")
                               .arg(kMaxAppNameChars));

        Request r;
        r.kind   = Request::Intent;
        r.intent = ShellIntents::kAppLaunchIntent;
        r.params.insert(ShellIntents::kAppLaunchParam, tail);
        return r;
    }

    if (host != QStringLiteral("intent"))
        return invalid(QStringLiteral("unknown form 'basecamp://%1/…'").arg(host));

    if (tail.isEmpty())
        return invalid(QStringLiteral("basecamp://intent/ needs an intent name"));
    if (!logos::intent::isValidName(tail))
        return invalid(QStringLiteral("'%1' fails the intent-name grammar").arg(tail));

    Request r;
    r.kind   = Request::Intent;
    r.intent = tail;

    const QUrlQuery query(url);
    // Exactly one recognised key. An unknown one is refused rather than
    // ignored: a caller who wrote `?params=` and got silence would see a
    // provider reject an empty payload and have nothing pointing at the typo.
    for (const auto& item : query.queryItems()) {
        if (item.first != QStringLiteral("p"))
            return invalid(QStringLiteral("unknown query key '%1'").arg(item.first));
    }

    // ONE key, and one is what that has to mean. queryItemValue() answers with
    // the FIRST match when a key repeats, so the loop above passes `?p=A&p=B`
    // happily and the payload acted on is A while B is discarded in silence —
    // a caller writing one payload and a shell reading another, which is the
    // ambiguity this parser exists to refuse rather than resolve.
    if (query.allQueryItemValues(QStringLiteral("p")).size() > 1)
        return invalid(QStringLiteral("'p' is given more than once"));

    const QString encoded = query.queryItemValue(QStringLiteral("p"),
                                                 QUrl::FullyDecoded);
    if (encoded.isEmpty()) {
        // No payload is legitimate — `basecamp.settings.open` takes none.
        return r;
    }

    const QByteArray decoded = QByteArray::fromBase64(
        encoded.toUtf8(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.isEmpty())
        return invalid(QStringLiteral("'p' is not valid base64url"));

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(decoded, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return invalid(QStringLiteral("'p' is not valid JSON: %1")
                           .arg(parseError.errorString()));
    if (!doc.isObject())
        return invalid(QStringLiteral("'p' must decode to a JSON object"));

    r.params = doc.object().toVariantMap();

    // The broker checks this too, immediately after submit. Checking here as
    // well is not redundant: params from a link are fully attacker-controlled,
    // and a refusal at this point names the URL in the log, where a broker
    // refusal names only a dispatch id.
    if (!logos::intent::isCanonicalPayload(r.params))
        return invalid(QStringLiteral("payload violates the intent payload rules"));

    return r;
}

} // namespace LinkUrl
