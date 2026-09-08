#pragma once

#include <QString>
#include <QUrl>
#include <QVariantMap>

// ── LinkUrl ──────────────────────────────────────────────────────────────────
//
// Turns a `basecamp://` URL into either "raise the window" or an intent to
// submit. Pure: no Qt GUI, no registry, no broker, no I/O — so the one piece of
// this feature that is entirely attacker-controlled is also the one that is
// cheapest to test exhaustively.
//
// Grammar, and nothing else is accepted:
//
//   basecamp://                       raise the window, nothing more
//   basecamp://app/<name>             sugar for basecamp.apps.launch {app: name}
//   basecamp://intent/<name>?p=<b64>  <b64> is base64url of a JSON object
//
// WHY base64url RATHER THAN QUERY PAIRS. Query parameters are strings, and the
// broker type-checks a payload against the provider's declared `params` — so
// `?amount=12.5` would arrive as the string "12.5" and be refused as the wrong
// type, with no way for the caller to express a number. A JSON object survives
// the round trip with its types intact, and it is one decoding path instead of
// a second ad-hoc one to get wrong.
namespace LinkUrl {

struct Request {
    enum Kind {
        Invalid,     // `error` says why; it goes to the log and nowhere else
        RaiseOnly,   // bare `basecamp://`
        Intent
    };

    Kind        kind = Invalid;
    QString     intent;
    QVariantMap params;
    QString     error;
};

// The scheme this build answers to. One definition, shared by the parser, the
// macOS plist check and the Linux/Windows registrars, so they cannot drift.
QString scheme();

Request parse(const QUrl& url);

// Convenience for the argv / socket paths, which carry a string. Rejects
// anything QUrl would have to guess at.
Request parse(const QString& rawUrl);

} // namespace LinkUrl
