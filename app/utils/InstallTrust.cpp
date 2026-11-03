#include "InstallTrust.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

#include <memory>
#include <utility>

#include <unicode/uspoof.h>

namespace logos::trust {

LogosKeys logosKeys()
{
    // The "Official Logos signing key" of logos-modules-release's README.
    return {{QStringLiteral(
                "did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlpUdEIzaU9FYVZDWFVLUWw0"
                "Sm9sR3V1MkhMb19iOUhSQ2V2RjRINm81aUkifQ")},
            {}};
}

QString logosDid()
{
    return logosKeys().dids.value(0);
}

QString logosKeyName()
{
    return QStringLiteral("logos");
}

QString logosName()
{
    return QStringLiteral("Logos");
}

QString logosUrl()
{
    return QStringLiteral("https://logos.co");
}

namespace {

constexpr auto kBase64Url = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

// The raw public key of a canonical did:jwk, or empty.
QByteArray publicKeyOf(const QString& did)
{
    const QString prefix = QStringLiteral("did:jwk:");
    if (!did.startsWith(prefix)) return {};
    const auto jwk = QByteArray::fromBase64Encoding(
        did.mid(prefix.size()).toLatin1(),
        kBase64Url | QByteArray::AbortOnBase64DecodingErrors);
    if (!jwk) return {};
    const QJsonObject key = QJsonDocument::fromJson(jwk.decoded).object();
    const QString x = key.value(QStringLiteral("x")).toString();
    const auto raw = QByteArray::fromBase64Encoding(
        x.toLatin1(), kBase64Url | QByteArray::AbortOnBase64DecodingErrors);
    if (!raw || raw.decoded.size() != 32) return {};
    // liblgx's encoding, byte for byte: anything else is another spelling.
    const QByteArray canonical = "{\"crv\":\"Ed25519\",\"kty\":\"OKP\",\"x\":\""
                                 + raw.decoded.toBase64(kBase64Url) + "\"}";
    if (prefix + QString::fromLatin1(canonical.toBase64(kBase64Url)) != did) return {};
    return raw.decoded;
}

Status signedBy(const QString& did, const QStringList& trusted)
{
    if (did.isEmpty())
        return Status::Unsigned;
    if (!isCanonicalDid(did))
        return Status::Unknown;
    return trusted.contains(did) ? Status::Trusted : Status::Untrusted;
}

// What an install would bring. A key spelled other than liblgx writes it can't
// be told apart from any other, a revoked one included, so it is refused.
Status signedForInstall(const QString& did, const QStringList& trusted)
{
    return !did.isEmpty() && !isCanonicalDid(did) ? Status::Invalid : signedBy(did, trusted);
}

// Unicode's Default_Ignorable_Code_Point: drawn as nothing, so a name can hide
// any number of them.
bool defaultIgnorable(char32_t c)
{
    return c == 0x00AD || c == 0x034F || c == 0x061C || (c >= 0x115F && c <= 0x1160)
        || (c >= 0x17B4 && c <= 0x17B5) || (c >= 0x180B && c <= 0x180F)
        || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E)
        || (c >= 0x2060 && c <= 0x206F) || c == 0x3164 || (c >= 0xFE00 && c <= 0xFE0F)
        || c == 0xFEFF || c == 0xFFA0 || (c >= 0xFFF0 && c <= 0xFFF8)
        || (c >= 0x1BCA0 && c <= 0x1BCA3) || (c >= 0x1D173 && c <= 0x1D17A)
        || (c >= 0xE0000 && c <= 0xE0FFF);
}

// What a reader sees of `text`, uncut: line breaks, and characters that are
// invisible, drawn blank or turn the text around, become spaces. Code point by
// code point, so nothing outside the BMP slips through as a surrogate pair.
QString cleaned(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const char32_t c : QStringView(text).toUcs4()) {
        switch (QChar::category(c)) {
        case QChar::Other_Control:        // line breaks among them
        case QChar::Other_Format:         // direction overrides, zero-width
        case QChar::Other_Surrogate:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
            out += QLatin1Char(' ');
            continue;
        default:
            break;
        }
        // U+2800, Braille's blank: a symbol to Unicode, a space to the eye.
        if (defaultIgnorable(c) || c == 0x2800)
            out += QLatin1Char(' ');
        else
            out += QString::fromUcs4(&c, 1);
    }
    return out.simplified();
}

// Unicode's skeleton of `text` (UTS #39): what it is drawn like, so strings
// that look alike share one. ICU's spoof checker, the one Qt itself links (as
// Chrome uses it). The text itself if the checker cannot run.
QString skeleton(const QString& text)
{
    static const std::unique_ptr<USpoofChecker, decltype(&uspoof_close)> checker = [] {
        UErrorCode status = U_ZERO_ERROR;
        USpoofChecker* opened = uspoof_open(&status);
        if (U_FAILURE(status)) {
            qWarning() << "InstallTrust: no spoof checker:" << u_errorName(status);
            opened = nullptr;
        }
        return std::unique_ptr<USpoofChecker, decltype(&uspoof_close)>(opened, &uspoof_close);
    }();
    if (!checker || text.isEmpty())
        return text;
    const auto* source = reinterpret_cast<const UChar*>(text.utf16());
    UErrorCode status = U_ZERO_ERROR;
    const int32_t size = uspoof_getSkeleton(checker.get(), 0, source, int32_t(text.size()),
                                            nullptr, 0, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR)
        return text;
    QString out(size, Qt::Uninitialized);
    status = U_ZERO_ERROR;
    uspoof_getSkeleton(checker.get(), 0, source, int32_t(text.size()),
                       reinterpret_cast<UChar*>(out.data()), size, &status);
    return U_SUCCESS(status) ? out : text;
}

QString lettersAndDigits(const QString& text)
{
    QString out;
    for (const char32_t c : QStringView(text).toUcs4()) {
        if (QChar::isLetterOrNumber(c))
            out += QString::fromUcs4(&c, 1);
    }
    return out;
}

// What `text` is drawn like, then lowercased and drawn again (lowercasing
// makes lookalikes of its own), with only letters and digits kept.
QString drawnKey(const QString& text)
{
    return lettersAndDigits(skeleton(skeleton(text).toCaseFolded()).toCaseFolded());
}

// A name as a reader sees it, case aside, read three ways: as drawn; with
// capital I (drawn like l) read as i; and lowercased first, as UTS #39 does
// for names whose case does not count. Skeletons keep case, so each catches
// lookalikes the others miss: a Greek capital N lowercases to a v-lookalike,
// while a small Cherokee letter is a lookalike only as its capital. A name
// copies another if any reading matches.
struct Readings {
    QString asDrawn;
    QString capitalIAsI;
    QString lowercasedFirst;
};

Readings readingsOf(const QString& name)
{
    const QString text = cleaned(name).normalized(QString::NormalizationForm_KC);
    QString asI = text;
    for (QChar& c : asI) {
        if (c == u'I' || c == u'\u0399' || c == u'\u0406' || c == u'\u04C0')   // Latin, Greek, Cyrillic I, palochka
            c = u'i';
    }
    return {drawnKey(text), drawnKey(asI),
            lettersAndDigits(skeleton(text.toCaseFolded()).toCaseFolded())};
}

bool sameName(const Readings& a, const Readings& b)
{
    return (!a.asDrawn.isEmpty() && a.asDrawn == b.asDrawn)
        || (!a.capitalIAsI.isEmpty() && a.capitalIAsI == b.capitalIAsI)
        || (!a.lowercasedFirst.isEmpty() && a.lowercasedFirst == b.lowercasedFirst);
}

// Whether a word of `name` starts reading as Logos: "Logos Official",
// "LogosHub", "My L o g o s". A name only Logos's keys may go by.
bool claimsLogos(const QString& name)
{
    static const Readings logos = readingsOf(QStringLiteral("Logos"));
    const QString text = cleaned(name).normalized(QString::NormalizationForm_KC);
    QStringList words;
    QString word;
    for (const char32_t c : QStringView(text).toUcs4()) {
        if (QChar::isLetterOrNumber(c)) {
            word += QString::fromUcs4(&c, 1);
            continue;
        }
        if (!word.isEmpty()) words << word;
        word.clear();
    }
    if (!word.isEmpty()) words << word;
    for (int i = 0; i < words.size(); ++i) {
        const Readings from = readingsOf(words.mid(i).join(QLatin1Char(' ')));
        if (from.asDrawn.startsWith(logos.asDrawn) || from.capitalIAsI.startsWith(logos.capitalIAsI)
            || from.lowercasedFirst.startsWith(logos.lowercasedFirst))
            return true;
    }
    return false;
}

} // namespace

QString shown(const QString& text)
{
    QString out = cleaned(text);
    constexpr int kLongest = 64;
    if (out.size() <= kLongest) return out;
    const int cut = out.at(kLongest - 2).isHighSurrogate() ? kLongest - 2 : kLongest - 1;
    return out.left(cut) + QChar(0x2026);
}

bool isCanonicalDid(const QString& did)
{
    return !publicKeyOf(did).isEmpty();
}

QString fingerprint(const QString& did)
{
    const QByteArray key = publicKeyOf(did);
    if (key.isEmpty()) return {};
    const QString hex = QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha256).left(8).toHex().toUpper());
    QStringList groups;
    for (int i = 0; i < hex.size(); i += 4) groups << hex.mid(i, 4);
    return groups.join(QLatin1Char(' '));
}

QStringList trustedDids(const QVariantList& trustedKeys, const LogosKeys& logos)
{
    QStringList dids;
    bool logosTrusted = false;
    for (const QVariant& key : trustedKeys) {
        const QString did = key.toMap().value(QStringLiteral("did")).toString();
        if (logos.dids.contains(did) || logos.revoked.contains(did))
            logosTrusted = true;
        else if (!did.isEmpty() && !dids.contains(did))
            dids << did;
    }
    return logosTrusted ? logos.dids + dids : dids;
}

Package fromCatalog(const QString& name, const QString& version, const QString& rootHash,
                    const QVariantList& versions, const QStringList& trusted)
{
    Package package{name, version, Status::Unknown};
    if (rootHash.isEmpty()) return package;
    bool found = false;
    for (const QVariant& v : versions) {
        const QVariantMap entry = v.toMap();
        if (entry.value(QStringLiteral("rootHash")).toString() != rootHash
            || !entry.value(QStringLiteral("sourceAvailable"), true).toBool())
            continue;
        const QVariantMap manifest = entry.value(QStringLiteral("manifest")).toMap();
        // It installs under its manifest's name, which the download is held
        // to. Listed as one package and installing as another, or as one no
        // manifest names, it could take any package's place.
        if (manifest.value(QStringLiteral("name")).toString() != name)
            return Package{name, version, Status::Invalid};
        if (manifest.value(QStringLiteral("version")).toString() != version)
            continue;
        const QVariantMap signature = entry.value(QStringLiteral("signature")).toMap();
        const QString did = signature.value(QStringLiteral("did")).toString();
        if (found) {
            // Which of them the downloader takes is not ours to guess.
            if (did != package.did) return Package{name, version, Status::Unknown};
            continue;
        }
        const QVariantMap signer = signature.value(QStringLiteral("signer")).toMap();
        package.did = did;
        package.signerName = signer.value(QStringLiteral("name")).toString();
        package.signerUrl = signer.value(QStringLiteral("url")).toString();
        found = true;
    }
    if (found) package.status = signedForInstall(package.did, trusted);
    return package;
}

Package fromInstalled(const QString& name, const QString& version, const QVariantMap& record,
                      const QStringList& trusted)
{
    Package package{name, version};
    package.installed = true;
    if (record.value(QStringLiteral("installType")).toString() == QLatin1String("embedded")) {
        package.status = Status::Bundled;
        return package;
    }
    package.did = record.value(QStringLiteral("signerDid")).toString();
    package.status = record.isEmpty() ? Status::Unknown : signedBy(package.did, trusted);
    return package;
}

Package asReplacement(Package package, const QVariantMap& record, const QStringList& bundled,
                      const LogosKeys& logos)
{
    // Names compare as a case-insensitive file system does, where Wallet_UI
    // lands on wallet_ui.
    if (bundled.contains(package.name, Qt::CaseInsensitive) && !logos.dids.contains(package.did)) {
        package.status = Status::Invalid;
        package.reserved = true;
        return package;
    }
    if (record.value(QStringLiteral("installType")).toString() != QLatin1String("user"))
        return package;
    const QString had = record.value(QStringLiteral("signerDid")).toString();
    if (!isCanonicalDid(had)) {
        // Nobody Basecamp can name signed the copy on disk: only Logos may
        // take its place unasked.
        if (!package.did.isEmpty() && !logos.dids.contains(package.did))
            package.replacesUnverified = true;
        return package;
    }
    const bool logosBefore = logos.dids.contains(had) || logos.revoked.contains(had);
    if (had != package.did && !(logosBefore && logos.dids.contains(package.did)))
        package.replacesSignedBy = had;
    return package;
}

QVariantMap installedRecordOf(const QVariantList& installed, const QString& name)
{
    for (const QVariant& v : installed)
        if (v.toMap().value(QStringLiteral("name")).toString() == name)
            return v.toMap();
    for (const QVariant& v : installed)
        if (v.toMap().value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0)
            return v.toMap();
    return {};
}

QStringList packageModuleCallers()
{
    return {QStringLiteral("core"), QStringLiteral("package_manager_ui_backend")};
}

QStringList reservedNames(const QStringList& modules, const QStringList& apps)
{
    QStringList names = modules + packageModuleCallers();
    names << QStringLiteral("core_service");
    for (const QString& app : apps)
        names << app << app + QStringLiteral("_backend");
    return names;
}

QList<Package> judgePlan(const QString& name, const QVariantList& resolved,
                         const std::function<bool(const QVariantMap&)>& onDisk,
                         const std::function<QVariantList(const QVariantMap&)>& versionsOf,
                         const std::function<QVariantMap(const QVariantMap&)>& installedOf,
                         const QStringList& trusted, const QStringList& bundled,
                         const LogosKeys& logos)
{
    // A planned row: what is on disk already, judged by that copy, or what
    // the install would fetch, by the catalogue. `name` is always fetched: a
    // reinstall replaces the copy on disk.
    const auto judge = [&](const QVariantMap& row) {
        const QString rowName = row.value(QStringLiteral("name")).toString();
        const QString version = row.value(QStringLiteral("version")).toString();
        if (rowName == name || !onDisk(row))
            return asReplacement(fromCatalog(rowName, version, row.value(QStringLiteral("rootHash")).toString(),
                                             versionsOf(row), trusted),
                                 installedOf(row), bundled, logos);
        return fromInstalled(rowName, version, installedOf(row), trusted);
    };

    QList<Package> packages;
    QStringList judged;
    if (resolved.isEmpty())
        packages.append({name, {}, Status::Unknown});
    for (const QVariant& v : resolved) {
        const QVariantMap row = v.toMap();
        const QString rowName = row.value(QStringLiteral("name")).toString();
        if (rowName.isEmpty() || row.contains(QStringLiteral("error"))) {
            packages.append({rowName.isEmpty() ? name : rowName,
                             row.value(QStringLiteral("version")).toString(), Status::Unknown});
            continue;
        }
        judged << rowName;
        packages.append(judge(row));
    }
    // The optional packages offered and not chosen, as the offer would install them.
    for (const QVariant& v : resolved) {
        for (const QVariant& o : v.toMap().value(QStringLiteral("optionalDependencies")).toList()) {
            const QVariantMap offer = o.toMap();
            const QString offerName = offer.value(QStringLiteral("name")).toString();
            if (offerName.isEmpty() || offer.contains(QStringLiteral("error")) || judged.contains(offerName))
                continue;
            judged << offerName;
            const QVariantMap request = offer.value(QStringLiteral("request")).toMap();
            Package package = judge({
                {QStringLiteral("name"), offerName},
                {QStringLiteral("version"), offer.value(QStringLiteral("version"))},
                {QStringLiteral("rootHash"), offer.value(QStringLiteral("rootHash"))},
                {QStringLiteral("repositoryUrl"),
                 offer.value(QStringLiteral("repositoryUrl"), request.value(QStringLiteral("repositoryUrl")))}});
            package.offered = true;
            packages.append(package);
        }
    }
    return packages;
}

QString localPackagePath(const QString& path, bool fromPackageManager)
{
    if (!fromPackageManager || path.size() < 2)
        return {};
    // A network share: `\\server\…`, `//server/…`, `\\?\UNC\…`, mixed as Windows allows.
    const auto separator = [](QChar c) { return c == QLatin1Char('/') || c == QLatin1Char('\\'); };
    if (separator(path.at(0)) && separator(path.at(1)))
        return {};
    if (!QDir::isAbsolutePath(path) || !path.endsWith(QLatin1String(".lgx"), Qt::CaseInsensitive))
        return {};
    return path;
}

Package fromVerifiedFile(const QString& name, const QString& version,
                         const QVariantMap& verified, const QStringList& trusted)
{
    Package package{name, version};
    const bool isSigned = verified.value(QStringLiteral("isSigned")).toBool();
    if (!verified.value(QStringLiteral("packageValid")).toBool()
        || (isSigned && !verified.value(QStringLiteral("signatureValid")).toBool())) {
        package.status = Status::Invalid;
        return package;
    }
    if (isSigned) {
        package.did = verified.value(QStringLiteral("signerDid")).toString();
        package.signerName = verified.value(QStringLiteral("signerName")).toString();
        package.signerUrl = verified.value(QStringLiteral("signerUrl")).toString();
    }
    package.status = signedForInstall(package.did, trusted);
    return package;
}

QVariantList approvedPlan(const QString& name, const QVariantList& resolved,
                          const QList<Package>& judged)
{
    QHash<QString, Package> byName;
    for (const Package& p : judged)
        if (!p.offered && !byName.contains(p.name))
            byName.insert(p.name, p);
    QVariantList plan;
    for (const QVariant& v : resolved) {
        const QVariantMap row = v.toMap();
        const QString rowName = row.value(QStringLiteral("name")).toString();
        if (rowName.isEmpty() || row.contains(QStringLiteral("error")))
            continue;
        const Package package = byName.value(rowName);
        if (package.installed && rowName != name)
            continue;
        QVariantMap request{{QStringLiteral("name"), rowName},
                            {QStringLiteral("version"), row.value(QStringLiteral("version"))},
                            {QStringLiteral("rootHash"), row.value(QStringLiteral("rootHash"))},
                            {QStringLiteral("repositoryUrl"), row.value(QStringLiteral("repositoryUrl"))}};
        if (!package.installed
            && (package.status == Status::Trusted || package.status == Status::Untrusted))
            request.insert(QStringLiteral("signer"), package.did);
        if (row.value(QStringLiteral("optional")).toBool())
            request.insert(QStringLiteral("optional"), true);
        plan << request;
    }
    return plan;
}

namespace {

QString releaseOf(const QVariantMap& row)
{
    return QStringList{row.value(QStringLiteral("name")).toString(),
                       row.value(QStringLiteral("version")).toString(),
                       row.value(QStringLiteral("rootHash")).toString(),
                       row.value(QStringLiteral("repositoryUrl")).toString()}
        .join(QChar(0x1f));
}

} // namespace

QString notAsApproved(const QVariantList& fetched, const QVariantList& planned,
                      const QVariantList& approved)
{
    QSet<QString> releases;
    for (const QVariant& v : approved) releases.insert(releaseOf(v.toMap()));
    QSet<QString> arrived;
    QHash<QString, QString> failed;   // name -> why
    for (const QVariant& v : fetched) {
        const QVariantMap row = v.toMap();
        const QString error = row.value(QStringLiteral("error")).toString();
        if (!error.isEmpty()) {
            failed.insert(row.value(QStringLiteral("name")).toString(), error);
            continue;
        }
        if (!releases.contains(releaseOf(row)))
            return QStringLiteral("What this installs changed after you approved it (%1 v%2), so nothing "
                                  "was installed. Try again to review it.")
                .arg(row.value(QStringLiteral("name")).toString(), row.value(QStringLiteral("version")).toString());
        arrived.insert(releaseOf(row));
    }
    // All of it, or none: an optional package may be missing, nothing else.
    for (const QVariant& v : planned) {
        const QVariantMap row = v.toMap();
        if (row.value(QStringLiteral("optional")).toBool() || arrived.contains(releaseOf(row))) continue;
        const QString name = row.value(QStringLiteral("name")).toString();
        if (failed.contains(name))
            return QStringLiteral("%1: %2").arg(name, failed.value(name));
        return QStringLiteral("%1 v%2 could not be fetched as approved, so nothing was installed. "
                              "Try again to review it.")
            .arg(name, row.value(QStringLiteral("version")).toString());
    }
    return {};
}

QString signerChanged(const QVariantMap& verified, const QVariantList& approved, const QVariantMap& row)
{
    QString signer;
    for (const QVariant& v : approved)
        if (releaseOf(v.toMap()) == releaseOf(row))
            signer = v.toMap().value(QStringLiteral("signer")).toString();
    // A check that did not run proves nothing either way.
    const bool held = verified.value(QStringLiteral("packageValid")).toBool()
        && (signer.isEmpty()
                ? !verified.value(QStringLiteral("isSigned")).toBool()
                : verified.value(QStringLiteral("isSigned")).toBool()
                      && verified.value(QStringLiteral("signatureValid")).toBool()
                      && verified.value(QStringLiteral("signerDid")).toString() == signer);
    if (held) return {};
    return QStringLiteral("%1 v%2 is not signed as you approved it, so nothing was installed. "
                          "Try again to review it.")
        .arg(row.value(QStringLiteral("name")).toString(), row.value(QStringLiteral("version")).toString());
}

QStringList publisherKeys(const QString& did, const LogosKeys& logos)
{
    if (logos.dids.contains(did) || logos.revoked.contains(did))
        return logos.dids + logos.revoked;
    return {did};
}

bool requestsLocalFile(const QVariantMap& params)
{
    return !params.value(QStringLiteral("localPath")).toString().isEmpty();
}

QVariantMap fileIdentity(const QString& name, const QString& version,
                         const QVariantMap& verified, const QVariantMap& inspected,
                         const QString& sha256)
{
    const bool verifiedSigned = verified.value(QStringLiteral("isSigned")).toBool()
                                && verified.value(QStringLiteral("signatureValid")).toBool()
                                && verified.value(QStringLiteral("packageValid")).toBool();
    const QString signer = verifiedSigned ? verified.value(QStringLiteral("signerDid")).toString()
                                          : QString();
    const QString inspectedSigner =
        inspected.value(QStringLiteral("signatureStatus")).toString() == QLatin1String("signed")
            ? inspected.value(QStringLiteral("signerDid")).toString() : QString();
    const QString rootHash = inspected.value(QStringLiteral("rootHash")).toString();
    if (rootHash.isEmpty() || sha256.isEmpty() || inspected.contains(QStringLiteral("error"))
        || inspected.value(QStringLiteral("name")).toString() != name
        || inspected.value(QStringLiteral("version")).toString() != version
        || inspectedSigner != signer)
        return {};
    return {{QStringLiteral("rootHash"), rootHash}, {QStringLiteral("signer"), signer},
            {QStringLiteral("sha256"), sha256}};
}

QString sha256Of(const QString& path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

QString publisherName(const QString& displayName, const QString& did, const LogosKeys& logos)
{
    if (logos.dids.contains(did) || logos.revoked.contains(did)) return logosName();
    if (claimsLogos(displayName) || claimsLogos(shown(displayName))) return QString();
    return shown(displayName);
}

QString publisherUrl(const QString& url, const QString& did, const LogosKeys& logos)
{
    if (logos.dids.contains(did) || logos.revoked.contains(did)) return logosUrl();
    // The host is what the address claims to be; a path may mention anything.
    if (claimsLogos(QUrl::fromUserInput(url.trimmed()).host())) return QString();
    return shown(url);
}

QVariantMap summary(const QList<Package>& packages, const QVariantList& trustedKeys,
                    const QString& about, const LogosKeys& logos)
{
    // The names a publisher may not take with a key of its own: Logos's, and
    // those of the keys the user trusts.
    struct Claim {
        Readings readings;
        QString did;
        QString name;   // as the user knows it
    };
    // Logos's name is taken even with no key of its own to sign with.
    QList<Claim> claimed{{readingsOf(logosName()), QString(), logosName()}};
    for (const QString& did : logos.dids)
        claimed.append({readingsOf(logosName()), did, logosName()});
    for (const QVariant& v : trustedKeys) {
        const QVariantMap key = v.toMap();
        const QString did = key.value(QStringLiteral("did")).toString();
        const QString name = publisherName(key.value(QStringLiteral("displayName")).toString(), did, logos);
        if (!name.isEmpty() && name != logosName())
            claimed.append({readingsOf(name), did, name});
    }
    const auto impersonated = [&claimed, &logos](const Package& p) {
        const Readings written = readingsOf(p.signerName);
        const Readings drawn = readingsOf(shown(p.signerName));
        QString copied;
        for (const Claim& claim : claimed) {
            if (!sameName(claim.readings, written) && !sameName(claim.readings, drawn)) continue;
            if (claim.did == p.did) return QString();   // its own name
            if (copied.isEmpty()) copied = claim.name;
        }
        // Logos's name may not even be part of another key's.
        if (copied.isEmpty() && !logos.dids.contains(p.did)
            && (claimsLogos(p.signerName) || claimsLogos(shown(p.signerName))))
            copied = logosName();
        return copied;
    };

    // The name a trusted key was trusted by: Basecamp's for Logos, else the
    // keyring's.
    const auto trustedName = [&trustedKeys, &logos](const QString& did) {
        if (logos.dids.contains(did)) return logosName();
        for (const QVariant& v : trustedKeys) {
            const QVariantMap key = v.toMap();
            if (key.value(QStringLiteral("did")).toString() == did) {
                const QString name =
                    publisherName(key.value(QStringLiteral("displayName")).toString(), did, logos);
                if (!name.isEmpty()) return name;
            }
        }
        return QStringLiteral("key ") + fingerprint(did);
    };

    QVariantList attention;
    QVariantList publishers;
    QStringList publisherDids;
    QVariantMap byName;
    bool blocked = false;
    Package aboutPackage;   // `about`, if it waits on its signer being trusted
    for (Package p : packages) {
        // Signed with a key Logos revoked: as good as a signature that fails.
        if (!p.did.isEmpty() && logos.revoked.contains(p.did))
            p.status = Status::Invalid;
        QVariantMap verdict{{QStringLiteral("status"), statusName(p.status)}};
        if (p.status == Status::Trusted) {
            verdict[QStringLiteral("publisher")] = trustedName(p.did);
        } else if (p.status == Status::Untrusted) {
            verdict[QStringLiteral("publisher")] = shown(p.signerName);
            verdict[QStringLiteral("impersonates")] = impersonated(p);
        }
        // Who signed the copy it replaces, when someone else signs this one.
        const QString replaces = p.replacesSignedBy.isEmpty() ? QString() : trustedName(p.replacesSignedBy);
        if (p.reserved)
            verdict[QStringLiteral("reserved")] = true;
        if (!replaces.isEmpty())
            verdict[QStringLiteral("replaces")] = replaces;
        if (p.replacesUnverified)
            verdict[QStringLiteral("replacesUnverified")] = true;
        if (p.installed)
            verdict[QStringLiteral("installed")] = true;
        if (p.offered)
            verdict[QStringLiteral("offered")] = true;
        byName.insert(p.name, verdict);
        // A change of signer asks even when the new one is trusted.
        const bool changesHands = !replaces.isEmpty() || p.replacesUnverified;
        if ((p.status == Status::Trusted && !changesHands) || p.installed || p.offered)
            continue;
        blocked = blocked || p.status == Status::Invalid;
        const bool untrusted = p.status == Status::Untrusted;
        if (untrusted && p.name == about && !changesHands)
            aboutPackage = p;
        QVariantMap row{{QStringLiteral("name"), shown(p.name)},
                        {QStringLiteral("version"), shown(p.version)},
                        {QStringLiteral("status"), statusName(p.status)},
                        {QStringLiteral("did"), untrusted ? p.did : QString()},
                        {QStringLiteral("fingerprint"), untrusted ? fingerprint(p.did) : QString()},
                        {QStringLiteral("signerName"), shown(p.signerName)},
                        {QStringLiteral("signerUrl"), shown(p.signerUrl)}};
        if (p.reserved)
            row[QStringLiteral("reserved")] = true;
        if (!replaces.isEmpty())
            row[QStringLiteral("replaces")] = replaces;
        if (p.replacesUnverified)
            row[QStringLiteral("replacesUnverified")] = true;
        attention << row;
        if (!untrusted)
            continue;
        const int at = publisherDids.indexOf(p.did);
        if (at < 0) {
            publisherDids << p.did;
            publishers << QVariantMap{{QStringLiteral("did"), p.did},
                                      {QStringLiteral("fingerprint"), fingerprint(p.did)},
                                      {QStringLiteral("name"), shown(p.signerName)},
                                      {QStringLiteral("url"), shown(p.signerUrl)},
                                      {QStringLiteral("packages"), QStringList{shown(p.name)}},
                                      {QStringLiteral("impersonates"), impersonated(p)}};
            continue;
        }
        // One key, one entry: if any of its packages copies a trusted name,
        // the publisher does.
        QVariantMap publisher = publishers[at].toMap();
        publisher[QStringLiteral("packages")] =
            publisher.value(QStringLiteral("packages")).toStringList() << shown(p.name);
        if (publisher.value(QStringLiteral("name")).toString().isEmpty())
            publisher[QStringLiteral("name")] = shown(p.signerName);
        if (publisher.value(QStringLiteral("impersonates")).toString().isEmpty())
            publisher[QStringLiteral("impersonates")] = impersonated(p);
        publishers[at] = publisher;
    }

    // "Trust & install" trusts who signed what the dialog is about, and
    // nobody else; never a publisher copying a trusted name.
    const QString aboutDid = aboutPackage.did;
    QVariantMap trust;
    for (const QVariant& v : std::as_const(publishers)) {
        const QVariantMap publisher = v.toMap();
        if (!aboutDid.isEmpty() && publisher.value(QStringLiteral("did")).toString() == aboutDid
            && publisher.value(QStringLiteral("impersonates")).toString().isEmpty()) {
            trust = publisher;
            // As `about` names it, not a dependency it also signed.
            trust[QStringLiteral("name")] = shown(aboutPackage.signerName);
            trust[QStringLiteral("url")] = shown(aboutPackage.signerUrl);
        }
    }
    // Trusting one of Logos's keys trusts them all.
    const auto samePublisher = [&logos, &aboutDid](const QString& did) {
        return did == aboutDid || (logos.dids.contains(did) && logos.dids.contains(aboutDid));
    };
    bool coversAll = !trust.isEmpty();
    for (const QVariant& v : std::as_const(attention))
        coversAll = coversAll && samePublisher(v.toMap().value(QStringLiteral("did")).toString())
            && !v.toMap().contains(QStringLiteral("replaces"))
            && !v.toMap().contains(QStringLiteral("replacesUnverified"));
    return {{QStringLiteral("needsPrompt"), !attention.isEmpty()},
            {QStringLiteral("blocked"), blocked},
            {QStringLiteral("canTrust"), !trust.isEmpty()},
            {QStringLiteral("trustCoversAll"), coversAll},
            {QStringLiteral("trust"), trust},
            {QStringLiteral("packages"), attention},
            {QStringLiteral("publishers"), publishers},
            {QStringLiteral("byName"), byName}};
}

QList<Package> unchecked(QList<Package> packages)
{
    for (Package& p : packages)
        if (p.status == Status::Trusted || p.status == Status::Untrusted)
            p.status = Status::Unknown;
    return packages;
}

QString planIdentity(const QVariantList& resolved)
{
    QStringList rows;
    for (const QVariant& v : resolved) {
        const QVariantMap row = v.toMap();
        rows << QStringList{row.value(QStringLiteral("name")).toString(),
                            row.value(QStringLiteral("version")).toString(),
                            row.value(QStringLiteral("rootHash")).toString(),
                            row.value(QStringLiteral("repositoryUrl")).toString(),
                            row.value(QStringLiteral("error")).toString()}
                    .join(QChar(0x1f));
    }
    return rows.join(QChar(0x1e));
}

QString keyNameFor(const QString& did)
{
    if (logosKeys().dids.contains(did))
        return logosKeyName();
    const QByteArray hash = QCryptographicHash::hash(did.toUtf8(), QCryptographicHash::Sha256);
    return QStringLiteral("publisher-") + QString::fromLatin1(hash.toHex().left(16));
}

QString statusName(Status status)
{
    switch (status) {
    case Status::Trusted:   return QStringLiteral("trusted");
    case Status::Untrusted: return QStringLiteral("untrusted");
    case Status::Unsigned:  return QStringLiteral("unsigned");
    case Status::Unknown:   return QStringLiteral("unknown");
    case Status::Invalid:   return QStringLiteral("invalid");
    case Status::Bundled:   return QStringLiteral("bundled");
    }
    return {};
}

} // namespace logos::trust
