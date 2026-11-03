// srcdeps: utils/InstallTrust.cpp
// libdeps: ICU::i18n ICU::uc
//
// Unit tests for trust at install (app/utils/InstallTrust.{h,cpp}): how each
// package an install brings is judged from the catalogue, or from verifying a
// local file, and what the install dialogs are told to show.

#include "../app/utils/InstallTrust.h"

#include <QtTest/QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QTemporaryDir>

using namespace logos::trust;

namespace {

// Canonical did:jwk of two made-up Ed25519 keys (bytes 1..32 and 101..132),
// spelled the way liblgx spells them.
const QString kOtherDid = QStringLiteral(
    "did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IkFRSURCQVVHQndnSkNnc01EUTRQRUJFU0V4"
    "UVZGaGNZR1JvYkhCMGVIeUEifQ");
const QString kThirdDid = QStringLiteral(
    "did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlpXWm5hR2xxYTJ4dGJtOXdjWEp6ZEhWMmQz"
    "aDVlbnQ4Zlg1X2dJR0NnNFEifQ");

QVariantMap release(const QString& name, const QString& version, const QString& rootHash, const QString& did = {},
                    const QString& signerName = {}, const QString& signerUrl = {})
{
    QVariantMap entry{{QStringLiteral("rootHash"), rootHash},
                      {QStringLiteral("manifest"), QVariantMap{{QStringLiteral("name"), name},
                                                               {QStringLiteral("version"), version}}}};
    if (!did.isEmpty()) {
        entry[QStringLiteral("signature")] = QVariantMap{
            {QStringLiteral("did"), did},
            {QStringLiteral("signer"), QVariantMap{{QStringLiteral("name"), signerName},
                                                   {QStringLiteral("url"), signerUrl}}}};
    }
    return entry;
}

QVariantMap verified(bool isSigned, bool signatureValid, bool packageValid, const QString& did = {})
{
    return {{QStringLiteral("isSigned"), isSigned},
            {QStringLiteral("signatureValid"), signatureValid},
            {QStringLiteral("packageValid"), packageValid},
            {QStringLiteral("signerDid"), did},
            {QStringLiteral("signerName"), QStringLiteral("Someone")},
            {QStringLiteral("signerUrl"), QStringLiteral("https://someone.example")}};
}

QString didOfJwk(const QByteArray& json, QByteArray::Base64Options options =
                     QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
{
    return QStringLiteral("did:jwk:") + QString::fromLatin1(json.toBase64(options));
}

QVariantMap planRow(const QString& name, const QString& version, const QString& rootHash)
{
    return {{QStringLiteral("name"), name}, {QStringLiteral("version"), version},
            {QStringLiteral("rootHash"), rootHash},
            {QStringLiteral("repositoryUrl"), QStringLiteral("https://repo.example")}};
}

QStringList names(const QVariantList& rows)
{
    QStringList out;
    for (const QVariant& row : rows)
        out << row.toMap().value(QStringLiteral("name")).toString();
    return out;
}

} // namespace

class InstallTrustTest : public QObject
{
    Q_OBJECT

private slots:
    // The built-in key is one string split over two literals: a slip there
    // would make every Logos package read as untrusted.
    void theLogosKeyIsAnEd25519PublicKey()
    {
        QVERIFY(logosDid().startsWith(QStringLiteral("did:jwk:")));
        const QByteArray jwk = QByteArray::fromBase64(
            logosDid().mid(8).toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        const QJsonObject key = QJsonDocument::fromJson(jwk).object();
        QCOMPARE(key.value(QStringLiteral("kty")).toString(), QStringLiteral("OKP"));
        QCOMPARE(key.value(QStringLiteral("crv")).toString(), QStringLiteral("Ed25519"));
        QCOMPARE(QByteArray::fromBase64(key.value(QStringLiteral("x")).toString().toLatin1(),
                                        QByteArray::Base64UrlEncoding).size(), 32);
        QVERIFY(isCanonicalDid(logosDid()));
    }

    // A key is shown by a fingerprint of the key itself: every Ed25519 did:jwk
    // starts with the same 22 characters, and its last few can be chosen.
    void aKeyIsShownByItsOwnFingerprint()
    {
        QCOMPARE(fingerprint(logosDid()), QStringLiteral("4BBC C11D CB35 F9FD"));
        QCOMPARE(fingerprint(kOtherDid), QStringLiteral("AE21 6C2E F524 7A37"));
        // The other key, with a member added so the DID ends like Logos's.
        const QString lookalike = didOfJwk(
            "{\"crv\":\"Ed25519\",\"kty\":\"OKP\",\"x\":\"AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA\","
            "\"y\":\"Ao5iI\"}");
        QCOMPARE(lookalike.right(8), logosDid().right(8));
        QCOMPARE(fingerprint(lookalike), QString());
    }

    // liblgx finds the key in a DID leniently. Anything but its own spelling
    // of a key names no signer Basecamp will vouch for.
    void anotherSpellingOfAKeyIsNotThatKey()
    {
        const QByteArray x = "ZTtB3iOEaVCXUKQl4JolGuu2HLo_b9HRCevF4H6o5iI";   // Logos's key
        const QStringList spellings{
            didOfJwk("{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"" + x + "\"}"),
            didOfJwk("{\"crv\":\"Ed25519\",\"kty\":\"OKP\",\"x\":\"" + x + "\",\"y\":\"1\"}"),
            didOfJwk("{\"crv\":\"Ed25519\",\"kty\":\"OKP\",\"x\":\"" + x + "\"}",
                     QByteArray::Base64UrlEncoding),   // padded
            didOfJwk("{\"crv\":\"Ed25519\",\"kty\":\"OKP\", \"x\":\"" + x + "\"}"),
            QStringLiteral("did:jwk:not base64!"),
            QStringLiteral("did:key:z6Mk")};
        // Not that key, nor any it could pass for, a revoked one included:
        // what an install would bring is refused. It used to be "couldn't
        // check", which can still be installed. An installed copy says so.
        for (const QString& did : spellings) {
            QVERIFY2(!isCanonicalDid(did), qPrintable(did));
            QCOMPARE(fromVerifiedFile(QStringLiteral("app"), {}, verified(true, true, true, did),
                                      {did, logosDid()}).status, Status::Invalid);
            QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"),
                                 {release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"), did)}, {did})
                         .status, Status::Invalid);
            QCOMPARE(fromInstalled(QStringLiteral("app"), {}, {{QStringLiteral("installType"), QStringLiteral("user")},
                                                               {QStringLiteral("signerDid"), did}}, {did})
                         .status, Status::Unknown);
        }
    }

    void theReleaseThePlanNamesDecides()
    {
        const QVariantList versions{
            release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("old"), logosDid(), QStringLiteral("Logos")),
            release(QStringLiteral("app"), QStringLiteral("2.0.0"), QStringLiteral("new"), kOtherDid, QStringLiteral("Other"),
                    QStringLiteral("https://other.example")),
            release(QStringLiteral("app"), QStringLiteral("3.0.0"), QStringLiteral("bare"))};
        const QStringList trusted{logosDid()};

        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("old"),
                             versions, trusted).status, Status::Trusted);

        const Package other = fromCatalog(QStringLiteral("app"), QStringLiteral("2.0.0"),
                                          QStringLiteral("new"), versions, trusted);
        QCOMPARE(other.status, Status::Untrusted);
        QCOMPARE(other.did, kOtherDid);
        QCOMPARE(other.signerName, QStringLiteral("Other"));
        QCOMPARE(other.signerUrl, QStringLiteral("https://other.example"));

        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("3.0.0"), QStringLiteral("bare"),
                             versions, trusted).status, Status::Unsigned);
        // Not in the catalogue we have: who signed it is unknown, which is
        // not the same as nobody.
        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("gone"),
                             versions, trusted).status, Status::Unknown);
        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), {}, versions, trusted).status,
                 Status::Unknown);
    }

    // The regression. A catalogue lists the release the plan downloads
    // unsigned, and another version with the same content hash "signed by
    // Logos", which nobody downloads and so nobody checks. Matching the hash
    // alone read the plan as Logos's.
    void aReleaseNobodyDownloadsCannotVouchForOneThatIs()
    {
        const QVariantList versions{
            release(QStringLiteral("delivery_module"), QStringLiteral("9.0.1-a"), QStringLiteral("H"), logosDid(), QStringLiteral("Logos")),
            release(QStringLiteral("delivery_module"), QStringLiteral("9.0.0"), QStringLiteral("H"))};
        QCOMPARE(fromCatalog(QStringLiteral("delivery_module"), QStringLiteral("9.0.0"),
                             QStringLiteral("H"), versions, {logosDid()}).status,
                 Status::Unsigned);
    }

    // The same release listed twice with different signers: which one the
    // downloader holds the file to is not Basecamp's to guess.
    void releasesThatDisagreeAboutTheSignerAreUnknown()
    {
        const QVariantList disagree{
            release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"), logosDid()),
            release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"))};
        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"),
                             disagree, {logosDid()}).status, Status::Unknown);

        const QVariantList agree{
            release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"), logosDid()),
            release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"), logosDid())};
        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"),
                             agree, {logosDid()}).status, Status::Trusted);
    }

    // A release the download source cannot serve is never the one fetched.
    void aReleaseTheSourceCannotServeIsNotTheOne()
    {
        QVariantMap unserved = release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"), logosDid());
        unserved[QStringLiteral("sourceAvailable")] = false;
        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"),
                             {unserved}, {logosDid()}).status, Status::Unknown);
        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"),
                             {unserved, release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("H"))},
                             {logosDid()}).status, Status::Unsigned);
    }

    // A local file is judged by verifying it, not by what it says.
    void aLocalFileIsJudgedByItsVerification()
    {
        const QStringList trusted{logosDid()};
        QCOMPARE(fromVerifiedFile(QStringLiteral("app"), {}, verified(true, true, true, logosDid()),
                                  trusted).status, Status::Trusted);
        const Package other = fromVerifiedFile(QStringLiteral("app"), {},
                                               verified(true, true, true, kOtherDid), trusted);
        QCOMPARE(other.status, Status::Untrusted);
        QCOMPARE(other.signerName, QStringLiteral("Someone"));
        QCOMPARE(fromVerifiedFile(QStringLiteral("app"), {}, verified(false, false, true), trusted).status,
                 Status::Unsigned);
        QCOMPARE(fromVerifiedFile(QStringLiteral("app"), {}, verified(true, false, true, logosDid()),
                                  trusted).status, Status::Invalid);
        QCOMPARE(fromVerifiedFile(QStringLiteral("app"), {}, verified(false, false, false), trusted).status,
                 Status::Invalid);
        QCOMPARE(fromVerifiedFile(QStringLiteral("app"), {}, QVariantMap{}, trusted).status,
                 Status::Invalid);
    }

    // What package_manager_ui must find in the file before installing it: the
    // root hash and signer Basecamp read, and only when both of Basecamp's
    // reads agree. A file that changed between them, or names no hash to
    // hold it to, gets nothing: the dialog then blocks it.
    void aLocalFileIsHeldToWhatBasecampRead()
    {
        const auto inspected = [](const QString& status, const QString& did,
                                  const QString& rootHash = QStringLiteral("h1")) {
            return QVariantMap{{QStringLiteral("name"), QStringLiteral("app")},
                               {QStringLiteral("version"), QStringLiteral("1.0.0")},
                               {QStringLiteral("rootHash"), rootHash},
                               {QStringLiteral("signatureStatus"), status},
                               {QStringLiteral("signerDid"), did}};
        };
        const QString app = QStringLiteral("app");
        const QString v1 = QStringLiteral("1.0.0");
        const QString h1 = QStringLiteral("h1");
        const QString f1 = QStringLiteral("f1");   // the whole file's SHA-256

        QCOMPARE(fileIdentity(app, v1, verified(true, true, true, kOtherDid),
                              inspected(QStringLiteral("signed"), kOtherDid), f1),
                 (QVariantMap{{QStringLiteral("rootHash"), h1}, {QStringLiteral("signer"), kOtherDid},
                              {QStringLiteral("sha256"), f1}}));
        QCOMPARE(fileIdentity(app, v1, verified(false, false, true), inspected(QStringLiteral("unsigned"), {}), f1),
                 (QVariantMap{{QStringLiteral("rootHash"), h1}, {QStringLiteral("signer"), QString()},
                              {QStringLiteral("sha256"), f1}}));

        // The two reads disagree: it changed while being checked.
        QVERIFY(fileIdentity(app, v1, verified(true, true, true, kOtherDid),
                             inspected(QStringLiteral("signed"), kThirdDid), f1).isEmpty());
        QVERIFY(fileIdentity(app, v1, verified(false, false, true),
                             inspected(QStringLiteral("signed"), kOtherDid), f1).isEmpty());
        QVERIFY(fileIdentity(app, QStringLiteral("2.0.0"), verified(false, false, true),
                             inspected(QStringLiteral("unsigned"), {}), f1).isEmpty());
        QVERIFY(fileIdentity(QStringLiteral("other"), v1, verified(false, false, true),
                             inspected(QStringLiteral("unsigned"), {}), f1).isEmpty());
        // Nothing to hold it to: no root hash, or the file could not be read whole.
        QVERIFY(fileIdentity(app, v1, verified(false, false, true),
                             inspected(QStringLiteral("unsigned"), {}, {}), f1).isEmpty());
        QVERIFY(fileIdentity(app, v1, verified(false, false, true),
                             inspected(QStringLiteral("unsigned"), {}), {}).isEmpty());
        QVariantMap unreadable = inspected(QStringLiteral("unsigned"), {});
        unreadable[QStringLiteral("error")] = QStringLiteral("Failed to load LGX package");
        QVERIFY(fileIdentity(app, v1, verified(false, false, true), unreadable, f1).isEmpty());
    }

    // The whole file, byte for byte: what package_manager_ui must find again.
    void aFileIsIdentifiedByTheSha256OfAllOfIt()
    {
        QTemporaryDir dir;
        QFile file(dir.filePath(QStringLiteral("app.lgx")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("abc");
        file.close();
        QCOMPARE(sha256Of(file.fileName()),
                 QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QCOMPARE(sha256Of(dir.filePath(QStringLiteral("gone.lgx"))), QString());
    }

    // What a plan would bring: everything not on disk, and nothing quietly
    // dropped. A failed resolution used to read as "all trusted".
    void aPlanIsJudgedRowByRowAndNothingIsLeftOut()
    {
        const auto onDisk = [](const QVariantMap&) { return false; };
        const auto versionsOf = [](const QVariantMap& row) {
            return QVariantList{release(row.value(QStringLiteral("name")).toString(), row.value(QStringLiteral("version")).toString(),
                                        row.value(QStringLiteral("rootHash")).toString(), logosDid())};
        };
        const auto installedOf = [](const QVariantMap&) { return QVariantMap{}; };
        const QStringList trusted{logosDid()};

        const QList<Package> plan = judgePlan(
            QStringLiteral("app"),
            {planRow(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"))},
            onDisk, versionsOf, installedOf, trusted);
        QCOMPARE(plan.size(), 1);
        QCOMPARE(plan.first().status, Status::Trusted);
        QVERIFY(!plan.first().installed);

        const QList<Package> failed = judgePlan(
            QStringLiteral("app"),
            {planRow(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a")),
             QVariantMap{{QStringLiteral("name"), QStringLiteral("missing")},
                         {QStringLiteral("error"), QStringLiteral("no candidate")}}},
            onDisk, versionsOf, installedOf, trusted);
        QCOMPARE(failed.size(), 2);
        QCOMPARE(failed.last().name, QStringLiteral("missing"));
        QCOMPARE(failed.last().status, Status::Unknown);
        QCOMPARE(summary(failed, {}).value(QStringLiteral("needsPrompt")).toBool(), true);

        const QList<Package> nothing = judgePlan(QStringLiteral("app"), {}, onDisk, versionsOf,
                                                 installedOf, trusted);
        QCOMPARE(nothing.size(), 1);
        QCOMPARE(nothing.first().status, Status::Unknown);
    }

    // Confirming a plan approves exactly the releases it judged: each pinned
    // to its version, root hash, repository and the key it was judged signed
    // by, so package_manager_ui can install nothing else. What stays on disk
    // is not fetched; the package asked for always is.
    void approvingAPlanPinsWhatItWouldFetch()
    {
        QVariantMap opt = planRow(QStringLiteral("opt"), QStringLiteral("1.1.0"), QStringLiteral("o"));
        opt[QStringLiteral("optional")] = true;
        const QVariantList resolved{
            planRow(QStringLiteral("dep"), QStringLiteral("1.0.0"), QStringLiteral("d")),
            planRow(QStringLiteral("kept"), QStringLiteral("3.0.0"), QStringLiteral("k")),
            opt,
            QVariantMap{{QStringLiteral("name"), QStringLiteral("broken")},
                        {QStringLiteral("error"), QStringLiteral("no candidate")}},
            planRow(QStringLiteral("app"), QStringLiteral("2.0.0"), QStringLiteral("a"))};
        const auto onDisk = [](const QVariantMap& row) {
            return row.value(QStringLiteral("name")).toString() == QLatin1String("kept");
        };
        const auto versionsOf = [](const QVariantMap& row) {
            const QString name = row.value(QStringLiteral("name")).toString();
            const QString did = name == QLatin1String("dep") ? kOtherDid
                              : name == QLatin1String("app") ? logosDid() : QString();
            return QVariantList{release(row.value(QStringLiteral("name")).toString(), row.value(QStringLiteral("version")).toString(),
                                        row.value(QStringLiteral("rootHash")).toString(), did)};
        };
        const auto installedOf = [](const QVariantMap&) { return QVariantMap{}; };
        const QList<Package> judged = judgePlan(QStringLiteral("app"), resolved, onDisk, versionsOf,
                                                installedOf, {logosDid()});

        const QVariantList plan = approvedPlan(QStringLiteral("app"), resolved, judged);
        QCOMPARE(names(plan), (QStringList{QStringLiteral("dep"), QStringLiteral("opt"), QStringLiteral("app")}));
        QCOMPARE(plan.at(0).toMap(),
                 (QVariantMap{{QStringLiteral("name"), QStringLiteral("dep")},
                              {QStringLiteral("version"), QStringLiteral("1.0.0")},
                              {QStringLiteral("rootHash"), QStringLiteral("d")},
                              {QStringLiteral("repositoryUrl"), QStringLiteral("https://repo.example")},
                              {QStringLiteral("signer"), kOtherDid}}));
        // Nobody signed it: nothing to pin it to but the release itself.
        QVERIFY(!plan.at(1).toMap().contains(QStringLiteral("signer")));
        QCOMPARE(plan.at(1).toMap().value(QStringLiteral("optional")).toBool(), true);
        QCOMPARE(plan.at(2).toMap().value(QStringLiteral("signer")).toString(), logosDid());
        QVERIFY(!plan.at(2).toMap().contains(QStringLiteral("optional")));

        // A reinstall fetches the package on disk again; the copy there says
        // nothing about who signed the one fetched, the release fetched does.
        const QVariantList again{planRow(QStringLiteral("app"), QStringLiteral("2.0.0"), QStringLiteral("a"))};
        const auto allOnDisk = [](const QVariantMap&) { return true; };
        const QVariantList reinstall = approvedPlan(
            QStringLiteral("app"), again,
            judgePlan(QStringLiteral("app"), again, allOnDisk, versionsOf, installedOf, {logosDid()}));
        QCOMPARE(names(reinstall), QStringList{QStringLiteral("app")});
        QCOMPARE(reinstall.first().toMap().value(QStringLiteral("signer")).toString(), logosDid());

        // Nothing resolved, nothing approved.
        QVERIFY(approvedPlan(QStringLiteral("app"), {}, {}).isEmpty());
    }

    // What is on disk already and stays is said too, judged by that copy --
    // and asks nothing: an unsigned module installed long ago does not make
    // this install one to "install anyway". One that ships with Basecamp is
    // the app's own.
    void whatIsInstalledAlreadyIsShownButAsksNothing()
    {
        const auto onDisk = [](const QVariantMap& row) {
            return row.value(QStringLiteral("name")).toString() != QLatin1String("app");
        };
        const auto versionsOf = [](const QVariantMap& row) {
            return QVariantList{release(row.value(QStringLiteral("name")).toString(), row.value(QStringLiteral("version")).toString(),
                                        row.value(QStringLiteral("rootHash")).toString(), logosDid())};
        };
        const auto installedOf = [](const QVariantMap& row) {
            const QString name = row.value(QStringLiteral("name")).toString();
            if (name == QLatin1String("storage_module"))
                return QVariantMap{{QStringLiteral("installType"), QStringLiteral("embedded")}};
            if (name == QLatin1String("signed_dep"))
                return QVariantMap{{QStringLiteral("installType"), QStringLiteral("user")},
                                   {QStringLiteral("signerDid"), logosDid()}};
            return QVariantMap{{QStringLiteral("installType"), QStringLiteral("user")}};
        };
        const QList<Package> plan = judgePlan(
            QStringLiteral("app"),
            {planRow(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a")),
             planRow(QStringLiteral("storage_module"), QStringLiteral("3.0.2"), QStringLiteral("b")),
             planRow(QStringLiteral("signed_dep"), QStringLiteral("1.0.0"), QStringLiteral("c")),
             planRow(QStringLiteral("loose_dep"), QStringLiteral("1.0.0"), QStringLiteral("d"))},
            onDisk, versionsOf, installedOf, {logosDid()});
        QCOMPARE(plan.size(), 4);
        QCOMPARE(plan[1].status, Status::Bundled);
        QVERIFY(plan[1].installed);
        QCOMPARE(plan[2].status, Status::Trusted);
        QCOMPARE(plan[3].status, Status::Unsigned);
        QVERIFY(plan[3].installed);

        const QVariantMap s = summary(plan, {});
        QCOMPARE(s.value(QStringLiteral("needsPrompt")).toBool(), false);
        QVERIFY(s.value(QStringLiteral("packages")).toList().isEmpty());
        const QVariantMap byName = s.value(QStringLiteral("byName")).toMap();
        QCOMPARE(byName.value(QStringLiteral("storage_module")).toMap().value(QStringLiteral("status")).toString(),
                 QStringLiteral("bundled"));
        QCOMPARE(byName.value(QStringLiteral("loose_dep")).toMap().value(QStringLiteral("status")).toString(),
                 QStringLiteral("unsigned"));
        QVERIFY(byName.value(QStringLiteral("loose_dep")).toMap().value(QStringLiteral("installed")).toBool());
        QVERIFY(!byName.value(QStringLiteral("app")).toMap().contains(QStringLiteral("installed")));
    }

    // An optional package offered but not chosen is judged too, as the offer
    // would install it, so choosing it is an informed choice -- and asks
    // nothing until chosen, when it is a planned row like any other.
    void anOptionalNotChosenIsShownButAsksNothing()
    {
        const auto onDisk = [](const QVariantMap&) { return false; };
        const auto versionsOf = [](const QVariantMap& row) {
            const QString name = row.value(QStringLiteral("name")).toString();
            return QVariantList{release(row.value(QStringLiteral("name")).toString(), row.value(QStringLiteral("version")).toString(),
                                        row.value(QStringLiteral("rootHash")).toString(),
                                        name == QLatin1String("opt_loose") ? QString() : logosDid())};
        };
        const auto installedOf = [](const QVariantMap&) { return QVariantMap{}; };
        QVariantMap app = planRow(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"));
        app[QStringLiteral("optionalDependencies")] = QVariantList{
            QVariantMap{{QStringLiteral("name"), QStringLiteral("opt_signed")},
                        {QStringLiteral("version"), QStringLiteral("1.0.0")},
                        {QStringLiteral("rootHash"), QStringLiteral("b")},
                        {QStringLiteral("repositoryUrl"), QStringLiteral("https://repo.example")}},
            QVariantMap{{QStringLiteral("name"), QStringLiteral("opt_loose")},
                        {QStringLiteral("version"), QStringLiteral("1.0.0")},
                        {QStringLiteral("rootHash"), QStringLiteral("c")},
                        {QStringLiteral("request"),
                         QVariantMap{{QStringLiteral("repositoryUrl"), QStringLiteral("https://repo.example")}}}},
            // Chosen, so already a row of the plan: judged once, as planned.
            QVariantMap{{QStringLiteral("name"), QStringLiteral("opt_chosen")},
                        {QStringLiteral("version"), QStringLiteral("1.0.0")},
                        {QStringLiteral("rootHash"), QStringLiteral("d")}}};
        const QList<Package> plan = judgePlan(
            QStringLiteral("app"),
            {app, planRow(QStringLiteral("opt_chosen"), QStringLiteral("1.0.0"), QStringLiteral("d"))},
            onDisk, versionsOf, installedOf, {logosDid()});
        QCOMPARE(plan.size(), 4);
        QCOMPARE(plan[1].name, QStringLiteral("opt_chosen"));
        QVERIFY(!plan[1].offered);
        QCOMPARE(plan[2].name, QStringLiteral("opt_signed"));
        QCOMPARE(plan[2].status, Status::Trusted);
        QVERIFY(plan[2].offered);
        QCOMPARE(plan[3].name, QStringLiteral("opt_loose"));
        QCOMPARE(plan[3].status, Status::Unsigned);
        QVERIFY(plan[3].offered);

        const QVariantMap s = summary(plan, {});
        QCOMPARE(s.value(QStringLiteral("needsPrompt")).toBool(), false);
        const QVariantMap loose = s.value(QStringLiteral("byName")).toMap()
                                      .value(QStringLiteral("opt_loose")).toMap();
        QCOMPARE(loose.value(QStringLiteral("status")).toString(), QStringLiteral("unsigned"));
        QVERIFY(loose.value(QStringLiteral("offered")).toBool());
    }

    // Nothing to decide, but each row still says who signed it: before, a
    // fully trusted install showed nothing at all.
    void aFullyTrustedInstallAsksNothingButSaysWhoSignedIt()
    {
        const QVariantList keys{QVariantMap{{QStringLiteral("did"), logosDid()},
                                            {QStringLiteral("displayName"), QStringLiteral("claimed")}},
                                QVariantMap{{QStringLiteral("did"), kOtherDid},
                                            {QStringLiteral("displayName"), QStringLiteral("Acme")}}};
        const QVariantMap s = summary({{QStringLiteral("a"), {}, Status::Trusted, logosDid()},
                                       {QStringLiteral("b"), {}, Status::Trusted, logosDid()},
                                       {QStringLiteral("c"), {}, Status::Trusted, kOtherDid}}, keys);
        QCOMPARE(s.value(QStringLiteral("needsPrompt")).toBool(), false);
        QCOMPARE(s.value(QStringLiteral("blocked")).toBool(), false);
        QVERIFY(s.value(QStringLiteral("packages")).toList().isEmpty());
        QVERIFY(s.value(QStringLiteral("publishers")).toList().isEmpty());

        const QVariantMap byName = s.value(QStringLiteral("byName")).toMap();
        QCOMPARE(byName.size(), 3);
        // Logos goes by Basecamp's name for it, whatever its keyring entry says.
        QCOMPARE(byName.value(QStringLiteral("a")).toMap().value(QStringLiteral("status")).toString(),
                 QStringLiteral("trusted"));
        QCOMPARE(byName.value(QStringLiteral("a")).toMap().value(QStringLiteral("publisher")).toString(),
                 QStringLiteral("Logos"));
        QCOMPARE(byName.value(QStringLiteral("c")).toMap().value(QStringLiteral("publisher")).toString(),
                 QStringLiteral("Acme"));
    }

    // Every package judged has a row verdict, keyed by its planned name.
    void everyPackageGetsARowVerdict()
    {
        const QVariantMap s = summary({{QStringLiteral("app"), {}, Status::Untrusted, kOtherDid,
                                        QStringLiteral("Logos")},
                                       {QStringLiteral("loose"), {}, Status::Unsigned},
                                       {QStringLiteral("unchecked"), {}, Status::Unknown},
                                       {QStringLiteral("bad"), {}, Status::Invalid},
                                       {QStringLiteral("core"), {}, Status::Trusted, logosDid()}}, {});
        const QVariantMap byName = s.value(QStringLiteral("byName")).toMap();
        const auto field = [&byName](const char* name, const char* key) {
            return byName.value(QLatin1String(name)).toMap().value(QLatin1String(key)).toString();
        };
        QCOMPARE(field("app", "status"), QStringLiteral("untrusted"));
        QCOMPARE(field("app", "publisher"), QStringLiteral("Logos"));
        QCOMPARE(field("app", "impersonates"), QStringLiteral("Logos"));
        QCOMPARE(field("loose", "status"), QStringLiteral("unsigned"));
        QCOMPARE(field("unchecked", "status"), QStringLiteral("unknown"));
        QCOMPARE(field("bad", "status"), QStringLiteral("invalid"));
        QCOMPARE(field("core", "status"), QStringLiteral("trusted"));
    }

    // One prompt for the whole install: every package that is not trusted,
    // and each publisher not trusted yet once, with the packages it signed.
    void oneInstallIsOnePromptListingWhatIsNotTrusted()
    {
        const QVariantMap s = summary({{QStringLiteral("app"), {}, Status::Untrusted, kOtherDid,
                                        QStringLiteral("Other")},
                                       {QStringLiteral("dep"), {}, Status::Untrusted, kOtherDid},
                                       {QStringLiteral("loose"), {}, Status::Unsigned},
                                       {QStringLiteral("unchecked"), {}, Status::Unknown},
                                       {QStringLiteral("core"), {}, Status::Trusted, logosDid()}}, {});
        QCOMPARE(s.value(QStringLiteral("needsPrompt")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("blocked")).toBool(), false);
        QCOMPARE(names(s.value(QStringLiteral("packages")).toList()),
                 (QStringList{QStringLiteral("app"), QStringLiteral("dep"), QStringLiteral("loose"),
                              QStringLiteral("unchecked")}));

        const QVariantList publishers = s.value(QStringLiteral("publishers")).toList();
        QCOMPARE(publishers.size(), 1);
        const QVariantMap publisher = publishers.first().toMap();
        QCOMPARE(publisher.value(QStringLiteral("did")).toString(), kOtherDid);
        QCOMPARE(publisher.value(QStringLiteral("fingerprint")).toString(), fingerprint(kOtherDid));
        QCOMPARE(publisher.value(QStringLiteral("name")).toString(), QStringLiteral("Other"));
        QCOMPARE(publisher.value(QStringLiteral("impersonates")).toString(), QString());
        QCOMPARE(publisher.value(QStringLiteral("packages")).toStringList(),
                 (QStringList{QStringLiteral("app"), QStringLiteral("dep")}));
    }

    // A key that takes the name of Logos, or of a publisher the user trusts,
    // is flagged, and the dialogs do not offer to trust it.
    void aPublisherTakingATrustedNameIsFlagged()
    {
        const QVariantList keys{QVariantMap{{QStringLiteral("displayName"), QStringLiteral("Acme")},
                                            {QStringLiteral("did"), kThirdDid}},
                                QVariantMap{{QStringLiteral("displayName"), QStringLiteral("Nimbus")},
                                            {QStringLiteral("did"), QStringLiteral("did:jwk:nimbus")}},
                                QVariantMap{{QStringLiteral("displayName"), QStringLiteral("Mail")},
                                            {QStringLiteral("did"), QStringLiteral("did:jwk:mail")}}};
        const auto flagged = [&keys](const QString& claimed, const QString& did) {
            const QVariantMap s = summary({{QStringLiteral("app"), {}, Status::Untrusted, did, claimed}}, keys);
            return s.value(QStringLiteral("publishers")).toList().first().toMap()
                .value(QStringLiteral("impersonates")).toString();
        };
        QCOMPARE(flagged(QStringLiteral("Logos"), kOtherDid), QStringLiteral("Logos"));
        QCOMPARE(flagged(QStringLiteral("  LOGOS "), kOtherDid), QStringLiteral("Logos"));
        QCOMPARE(flagged(QStringLiteral("ａｃｍｅ"), kOtherDid), QStringLiteral("Acme"));   // full width
        // Logos itself, deleted from the keyring, is not pretending to be anyone.
        QCOMPARE(flagged(QStringLiteral("Logos"), logosDid()), QString());
        QCOMPARE(flagged(QStringLiteral("Other"), kOtherDid), QString());
        QCOMPARE(flagged(QStringLiteral("Logos Inc"), kOtherDid), QStringLiteral("Logos"));   // a word of it
        // As shown, too: cut where it is shown, what is left reads as Logos,
        // though the whole name does not.
        const QString padded = QStringLiteral("Logos") + QString(70, QLatin1Char('.')) + QStringLiteral("Labs");
        QCOMPARE(flagged(padded, kOtherDid), QStringLiteral("Logos"));
        QCOMPARE(summary({{QStringLiteral("app"), {}, Status::Untrusted, kOtherDid, padded}}, keys,
                         QStringLiteral("app"))
                     .value(QStringLiteral("canTrust")).toBool(), false);

        // A name that reads the same is the same: invisible characters (in the
        // BMP or not), lookalike letters from other scripts, digits and a
        // capital I passing for letters, spacing and punctuation.
        const QStringList lookalikes{
            QStringLiteral("Logos\u200B"), QStringLiteral("\uFEFFLogos"), QStringLiteral("Lo\u034Fgos"),
            QStringLiteral("Logos\uFE0F"), QStringLiteral("Lo\U000E0067gos"), QStringLiteral("Logos\u00AD"),
            QStringLiteral("L\u043Egos"), QStringLiteral("L\u03BFgos"), QStringLiteral("L0gos"),
            QStringLiteral("Iogos"), QStringLiteral("Lo gos"), QStringLiteral("Logos."),
            QStringLiteral("L\u0585g\u0585s"), QStringLiteral("Lo\u0581os"), QStringLiteral("L\u1D0Fgos"),
            QStringLiteral("L\u1040gos"), QStringLiteral("\u04C0ogos"),
            QStringLiteral("Logo\uABA5"), QStringLiteral("\uABAEogos")};   // Cherokee small letters
        for (const QString& name : lookalikes)
            QVERIFY2(flagged(name, kOtherDid) == QStringLiteral("Logos"), qPrintable(name));
        QCOMPARE(flagged(QStringLiteral("\u0410cme"), kOtherDid), QStringLiteral("Acme"));   // Cyrillic A
        // Case aside, for names with an i too, and capitals from other scripts.
        for (const QString& name : {QStringLiteral("NIMBUS"), QStringLiteral("nimbus"),
                                    QStringLiteral("N\u0406MBUS"), QStringLiteral("\u039DIMBUS")})
            QVERIFY2(flagged(name, kOtherDid) == QStringLiteral("Nimbus"), qPrintable(name));
        QCOMPARE(flagged(QStringLiteral("Nirnbus"), kOtherDid), QStringLiteral("Nimbus"));   // rn for m
        QCOMPARE(flagged(QStringLiteral("Nimble"), kOtherDid), QString());
        // i and l alike only where a capital I could be either: "Mall" is not "Mail".
        QCOMPARE(flagged(QStringLiteral("Mail"), kOtherDid), QStringLiteral("Mail"));
        QCOMPARE(flagged(QStringLiteral("Mall"), kOtherDid), QString());
    }

    // Only the Package Manager may name a file to check, and only a local
    // .lgx: opening a network share would contact it, before anyone clicks.
    void onlyALocalPackageFileIsOpened()
    {
        QCOMPARE(localPackagePath(QStringLiteral("/home/u/app.lgx"), true), QStringLiteral("/home/u/app.lgx"));
        QCOMPARE(localPackagePath(QStringLiteral("/home/u/APP.LGX"), true), QStringLiteral("/home/u/APP.LGX"));
        QCOMPARE(localPackagePath(QStringLiteral("/home/u/app.lgx"), false), QString());
        for (const QString& refused : {QStringLiteral("//server/share/app.lgx"),
                                       QStringLiteral("\\\\server\\share\\app.lgx"),
                                       QStringLiteral("/\\server/app.lgx"),
                                       QStringLiteral("relative/app.lgx"),
                                       QStringLiteral("/home/u/notes.txt"),
                                       QString()})
            QVERIFY2(localPackagePath(refused, true).isEmpty(), qPrintable(refused));
    }

    // What a publisher calls itself is shown as text, and nothing in it can
    // break the line, turn it around, or push what follows out of view.
    void aPublishersOwnTextIsMadeFitToShow()
    {
        QCOMPARE(shown(QStringLiteral("Acme\nTrusted by Logos")), QStringLiteral("Acme Trusted by Logos"));
        QCOMPARE(shown(QStringLiteral("Acme\u202Esgol")), QStringLiteral("Acme sgol"));   // right-to-left override
        QCOMPARE(shown(QStringLiteral("A\u200Bcme")), QStringLiteral("A cme"));           // zero width
        QCOMPARE(shown(QStringLiteral("  Acme   Inc ")), QStringLiteral("Acme Inc"));
        // Invisible outside the BMP too, and those that are not format characters.
        QCOMPARE(shown(QStringLiteral("Ac\U000E0067me")), QStringLiteral("Ac me"));   // tag character
        QCOMPARE(shown(QStringLiteral("Acme\uFE0F")), QStringLiteral("Acme"));         // variation selector
        QCOMPARE(shown(QStringLiteral("Ac\u034Fme")), QStringLiteral("Ac me"));        // combining grapheme joiner
        // Braille's blank is drawn as a space and folds like one, so a run of
        // them cannot push the rest of a name out of view.
        QCOMPARE(shown(QStringLiteral("Logos") + QString(58, QChar(0x2800)) + QStringLiteral("Labs")),
                 QStringLiteral("Logos Labs"));
        const QString longName = shown(QString(200, QLatin1Char('a')));
        QCOMPARE(longName.size(), 64);
        QVERIFY(longName.endsWith(QChar(0x2026)));
        // A cut never splits a character in two.
        const QString emoji = QString(62, QLatin1Char('a')) + QStringLiteral("\U0001F600\U0001F600");
        QVERIFY(!shown(emoji).at(shown(emoji).size() - 2).isHighSurrogate());

        const QVariantMap s = summary({{QStringLiteral("app\n"), {}, Status::Untrusted, kOtherDid,
                                        QStringLiteral("<b>Acme</b>\u202E")}}, {});
        const QVariantMap publisher = s.value(QStringLiteral("publishers")).toList().first().toMap();
        QCOMPARE(publisher.value(QStringLiteral("name")).toString(), QStringLiteral("<b>Acme</b>"));
        QCOMPARE(publisher.value(QStringLiteral("packages")).toStringList(), QStringList{QStringLiteral("app")});
    }

    // An installed copy is judged by package_manager's record of it: the
    // "Open with…" chooser says this of every installed provider.
    void anInstalledCopyIsJudgedByItsRecord()
    {
        const QStringList trusted{logosDid()};
        const auto judged = [&trusted](const QVariantMap& record) {
            return fromInstalled(QStringLiteral("app"), QStringLiteral("1.0.0"), record, trusted);
        };
        QCOMPARE(judged({{QStringLiteral("installType"), QStringLiteral("embedded")}}).status, Status::Bundled);
        QCOMPARE(judged({{QStringLiteral("installType"), QStringLiteral("user")},
                         {QStringLiteral("signerDid"), logosDid()}}).status, Status::Trusted);
        QCOMPARE(judged({{QStringLiteral("installType"), QStringLiteral("user")},
                         {QStringLiteral("signerDid"), kOtherDid}}).status, Status::Untrusted);
        QCOMPARE(judged({{QStringLiteral("installType"), QStringLiteral("user")}}).status, Status::Unsigned);
        QCOMPARE(judged({}).status, Status::Unknown);
        QVERIFY(judged({}).installed);

        // As the chooser shows it: Logos by name.
        const QVariantMap verdict = summary({judged({{QStringLiteral("installType"), QStringLiteral("user")},
                                                     {QStringLiteral("signerDid"), logosDid()}})}, {})
                                        .value(QStringLiteral("byName")).toMap()
                                        .value(QStringLiteral("app")).toMap();
        QCOMPARE(verdict.value(QStringLiteral("status")).toString(), QStringLiteral("trusted"));
        QCOMPARE(verdict.value(QStringLiteral("publisher")).toString(), QStringLiteral("Logos"));
    }

    // "Trust & install" is offered for who signed the package the dialog is
    // about, and trusts nobody else: it used to trust every signer in the
    // plan in one click. The other answer is a plain "Install" only when that
    // settles everything that asks, otherwise "Install anyway".
    void whatTrustingWouldSettle()
    {
        const auto flags = [](const QList<Package>& plan) {
            const QVariantMap s = summary(plan, {}, QStringLiteral("app"));
            return std::make_pair(s.value(QStringLiteral("canTrust")).toBool(),
                                  s.value(QStringLiteral("trustCoversAll")).toBool());
        };
        const Package app{QStringLiteral("app"), {}, Status::Untrusted, kOtherDid, QStringLiteral("Other")};
        const Package sameKey{QStringLiteral("dep"), {}, Status::Untrusted, kOtherDid};
        const Package dep{QStringLiteral("dep"), {}, Status::Untrusted, kThirdDid, QStringLiteral("Third")};
        const Package lookalike{QStringLiteral("fake"), {}, Status::Untrusted, kThirdDid, QStringLiteral("Logos")};
        const Package loose{QStringLiteral("loose"), {}, Status::Unsigned};
        QCOMPARE(flags({app, sameKey}), std::make_pair(true, true));
        QCOMPARE(flags({app, dep}), std::make_pair(true, false));
        QCOMPARE(flags({app, loose}), std::make_pair(true, false));
        QCOMPARE(flags({app, {QStringLiteral("unchecked"), {}, Status::Unknown}}), std::make_pair(true, false));
        QCOMPARE(flags({app, lookalike}), std::make_pair(true, false));
        // A dependency's publisher is never the one offered.
        QCOMPARE(flags({{QStringLiteral("app"), {}, Status::Trusted, logosDid()}, dep}), std::make_pair(false, false));
        QCOMPARE(flags({{QStringLiteral("app"), {}, Status::Unsigned}, dep}), std::make_pair(false, false));
        QCOMPARE(flags({{QStringLiteral("app"), {}, Status::Untrusted, kThirdDid, QStringLiteral("Logos")}}),
                 std::make_pair(false, false));
        QCOMPARE(flags({loose}), std::make_pair(false, false));
        // Installed already, or only offered, a package asks nothing.
        Package installed = loose;
        installed.installed = true;
        QCOMPARE(flags({app, installed}), std::make_pair(true, true));

        const QVariantMap s = summary({app, dep}, {}, QStringLiteral("app"));
        QCOMPARE(s.value(QStringLiteral("trust")).toMap().value(QStringLiteral("did")).toString(), kOtherDid);
        QCOMPARE(s.value(QStringLiteral("publishers")).toList().size(), 2);   // both still listed
        QVERIFY(summary({app}, {}).value(QStringLiteral("trust")).toMap().isEmpty());   // about nothing
    }

    // Logos's keys rotate. A keyring holding the key Logos used before still
    // stands for Logos, and so for its new key, and what either signed is
    // Logos's. A revoked key is still the user's yes to Logos but trusts
    // nothing itself, and what it signed can't be installed.
    void aLogosKeyOfAnyAgeStandsForLogos()
    {
        const QString oldKey = logosDid();
        const QVariantList holdsOld{QVariantMap{{QStringLiteral("name"), logosKeyName()},
                                                {QStringLiteral("did"), oldKey},
                                                {QStringLiteral("displayName"), logosName()}}};
        const LogosKeys rotated{{kThirdDid, oldKey}, {}};
        QCOMPARE(trustedDids(holdsOld, rotated), (QStringList{kThirdDid, oldKey}));
        QVERIFY(trustedDids({}, rotated).isEmpty());   // Logos deleted
        const auto byName = [](const QVariantMap& s, const char* key) {
            return s.value(QStringLiteral("byName")).toMap().value(QStringLiteral("app")).toMap()
                .value(QLatin1String(key)).toString();
        };
        QVariantMap s = summary({{QStringLiteral("app"), {}, Status::Trusted, kThirdDid}}, holdsOld,
                                QStringLiteral("app"), rotated);
        QCOMPARE(byName(s, "publisher"), QStringLiteral("Logos"));
        s = summary({{QStringLiteral("app"), {}, Status::Untrusted, oldKey, QStringLiteral("Logos")}}, {},
                    QStringLiteral("app"), rotated);
        QCOMPARE(byName(s, "impersonates"), QString());
        QCOMPARE(s.value(QStringLiteral("canTrust")).toBool(), true);

        const LogosKeys leaked{{kThirdDid}, {oldKey}};
        QCOMPARE(trustedDids(holdsOld, leaked), QStringList{kThirdDid});
        s = summary({{QStringLiteral("app"), {}, Status::Untrusted, oldKey, QStringLiteral("Logos")}}, holdsOld,
                    QStringLiteral("app"), leaked);
        QCOMPARE(byName(s, "status"), QStringLiteral("invalid"));
        QCOMPARE(s.value(QStringLiteral("blocked")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("canTrust")).toBool(), false);
    }

    // Without a keyring to check them against, signers are neither trusted
    // nor offered: a name a trusted publisher holds can't be told apart then.
    void withoutAKeyringNoSignerIsTrustedOrOffered()
    {
        const QList<Package> judged = unchecked({{QStringLiteral("app"), {}, Status::Untrusted, kOtherDid,
                                                  QStringLiteral("Acme")},
                                                 {QStringLiteral("core"), {}, Status::Trusted, logosDid()},
                                                 {QStringLiteral("loose"), {}, Status::Unsigned},
                                                 {QStringLiteral("bad"), {}, Status::Invalid}});
        QCOMPARE(judged[0].status, Status::Unknown);
        QCOMPARE(judged[1].status, Status::Unknown);
        QCOMPARE(judged[2].status, Status::Unsigned);
        QCOMPARE(judged[3].status, Status::Invalid);
        const QVariantMap s = summary(judged, {}, QStringLiteral("app"));
        QCOMPARE(s.value(QStringLiteral("canTrust")).toBool(), false);
        QVERIFY(s.value(QStringLiteral("publishers")).toList().isEmpty());
    }

    void anythingInvalidBlocksTheInstall()
    {
        const QVariantMap s = summary({{QStringLiteral("app"), {}, Status::Unsigned},
                                       {QStringLiteral("bad"), {}, Status::Invalid}}, {});
        QCOMPARE(s.value(QStringLiteral("needsPrompt")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("blocked")).toBool(), true);
    }

    // A verdict is about one plan: any row changing makes it another.
    void aPlanIsIdentifiedByEveryRow()
    {
        const QVariantList plan{planRow(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a")),
                                planRow(QStringLiteral("dep"), QStringLiteral("2.0.0"), QStringLiteral("b"))};
        QCOMPARE(planIdentity(plan), planIdentity(plan));
        QVariantList otherHash = plan;
        otherHash[1] = planRow(QStringLiteral("dep"), QStringLiteral("2.0.0"), QStringLiteral("c"));
        QVERIFY(planIdentity(otherHash) != planIdentity(plan));
        QVariantList otherRepo = plan;
        QVariantMap moved = plan[1].toMap();
        moved[QStringLiteral("repositoryUrl")] = QStringLiteral("https://elsewhere.example");
        otherRepo[1] = moved;
        QVERIFY(planIdentity(otherRepo) != planIdentity(plan));
        QVERIFY(planIdentity({plan.first()}) != planIdentity(plan));
    }

    void aPublisherIsTrustedUnderAStableSafeName()
    {
        QCOMPARE(keyNameFor(logosDid()), logosKeyName());
        const QString name = keyNameFor(kOtherDid);
        QCOMPARE(keyNameFor(kOtherDid), name);
        QVERIFY(name != keyNameFor(kThirdDid));
        QVERIFY(!name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
                && !name.contains(QStringLiteral("..")));
    }

    // The list Basecamp ships has a key to sign with now, every key spelled
    // as liblgx spells it, and none both good and revoked.
    void theShippedLogosKeysAreSound()
    {
        const LogosKeys keys = logosKeys();
        QVERIFY(!keys.dids.isEmpty());
        QCOMPARE(logosDid(), keys.dids.first());
        for (const QString& did : keys.dids + keys.revoked)
            QVERIFY2(isCanonicalDid(did), qPrintable(did));
        for (const QString& did : keys.revoked)
            QVERIFY2(!keys.dids.contains(did), qPrintable(did));
    }

    // Logos's name is taken even when Logos has no key left to sign with:
    // with every key revoked, any key used to be able to take it.
    void logosNameIsTakenEvenWithNoKey()
    {
        const LogosKeys noneLeft{{}, {logosDid()}};
        const QVariantMap s = summary({{QStringLiteral("app"), {}, Status::Untrusted, kOtherDid,
                                        QStringLiteral("Logos")}}, {}, QStringLiteral("app"), noneLeft);
        QCOMPARE(s.value(QStringLiteral("publishers")).toList().first().toMap()
                     .value(QStringLiteral("impersonates")).toString(), QStringLiteral("Logos"));
        QCOMPARE(s.value(QStringLiteral("canTrust")).toBool(), false);
    }

    // A key that isn't Logos's never reads as Logos, whoever put the name in
    // the keyring (another module, or the lgx CLI): it used to say "Signed by
    // Logos".
    void aKeyThatIsNotLogossNeverReadsAsLogos()
    {
        QCOMPARE(publisherName(QStringLiteral("Logos"), kOtherDid), QString());
        QCOMPARE(publisherName(QStringLiteral("\uFF2C\uFF4F\uFF47\uFF4F\uFF53"), kOtherDid), QString());   // full width
        QCOMPARE(publisherName(QStringLiteral("Logos Labs"), kOtherDid), QString());   // a word of it
        QCOMPARE(publisherName(QStringLiteral("Acme Labs"), kOtherDid), QStringLiteral("Acme Labs"));
        QCOMPARE(publisherName(QStringLiteral("anything"), logosDid()), QStringLiteral("Logos"));
        const QVariantList keys{QVariantMap{{QStringLiteral("did"), kOtherDid},
                                            {QStringLiteral("displayName"), QStringLiteral("Logos")}}};
        const QVariantMap s = summary({{QStringLiteral("app"), {}, Status::Trusted, kOtherDid}}, keys,
                                      QStringLiteral("app"));
        QCOMPARE(s.value(QStringLiteral("byName")).toMap().value(QStringLiteral("app")).toMap()
                     .value(QStringLiteral("publisher")).toString(),
                 QStringLiteral("key ") + fingerprint(kOtherDid));
    }

    // "Trust & install" stores the publisher as the package the dialog is
    // about names it: dependencies come first in a plan, and it used to take
    // a dependency's name for the same key.
    void trustGoesByTheNameOfWhatTheDialogIsAbout()
    {
        const QVariantMap s = summary({{QStringLiteral("dep"), {}, Status::Untrusted, kOtherDid,
                                        QStringLiteral("Other Deps Ltd")},
                                       {QStringLiteral("app"), {}, Status::Untrusted, kOtherDid,
                                        QStringLiteral("Other"), QStringLiteral("https://other.example")}},
                                      {}, QStringLiteral("app"));
        const QVariantMap trust = s.value(QStringLiteral("trust")).toMap();
        QCOMPARE(trust.value(QStringLiteral("name")).toString(), QStringLiteral("Other"));
        QCOMPARE(trust.value(QStringLiteral("url")).toString(), QStringLiteral("https://other.example"));
    }

    // Trusting Logos trusts every key it signs with, so it settles them all:
    // it used to say "anyway" for one signed with another of Logos's keys.
    void trustingLogosSettlesEveryLogosKey()
    {
        const LogosKeys rotated{{kThirdDid, logosDid()}, {}};
        const QVariantMap s = summary({{QStringLiteral("app"), {}, Status::Untrusted, logosDid(), QStringLiteral("Logos")},
                                       {QStringLiteral("dep"), {}, Status::Untrusted, kThirdDid, QStringLiteral("Logos")}},
                                      {}, QStringLiteral("app"), rotated);
        QCOMPARE(s.value(QStringLiteral("canTrust")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("trustCoversAll")).toBool(), true);
        QCOMPARE(publisherKeys(logosDid(), rotated), (QStringList{kThirdDid, logosDid()}));
        QCOMPARE(publisherKeys(kOtherDid, rotated), QStringList{kOtherDid});
    }

    // What App Manager fetched installs only as approved: all of it or none,
    // nothing outside the plan. A failed download used to install what had
    // come before it.
    void whatWasFetchedIsHeldToTheApproval()
    {
        QVariantMap optional = planRow(QStringLiteral("extra"), QStringLiteral("1.0.0"), QStringLiteral("e"));
        optional.insert(QStringLiteral("optional"), true);
        const QVariantList planned{planRow(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a")),
                                   planRow(QStringLiteral("dep"), QStringLiteral("2.0.0"), QStringLiteral("d")),
                                   optional};
        const QVariantList approved = planned;
        const auto failed = [](QVariantMap row, const QString& why) {
            row.insert(QStringLiteral("error"), why);
            return row;
        };
        QCOMPARE(notAsApproved(planned, planned, approved), QString());
        QCOMPARE(notAsApproved({planned[0], planned[1], failed(optional, QStringLiteral("gone"))}, planned, approved),
                 QString());
        QCOMPARE(notAsApproved({planned[0], failed(planned[1].toMap(), QStringLiteral("timed out"))}, planned, approved),
                 QStringLiteral("dep: timed out"));
        QVERIFY(notAsApproved({planned[0]}, planned, approved).startsWith(QStringLiteral("dep v2.0.0 could not be fetched")));
        QVERIFY(notAsApproved({planRow(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("other")),
                               planned[1]}, planned, approved)
                    .startsWith(QStringLiteral("What this installs changed after you approved it (app v1.0.0)")));
    }

    // A fetched file is signed exactly as approved, or not installed: App
    // Manager used to install whatever the downloader accepted.
    void aFetchedFileIsSignedAsApproved()
    {
        QVariantMap signedRow = planRow(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"));
        signedRow.insert(QStringLiteral("signer"), kOtherDid);
        const QVariantMap looseRow = planRow(QStringLiteral("loose"), QStringLiteral("1.0.0"), QStringLiteral("l"));
        const QVariantList approved{signedRow, looseRow};
        QCOMPARE(signerChanged(verified(true, true, true, kOtherDid), approved, signedRow), QString());
        QVERIFY(!signerChanged(verified(true, true, true, kThirdDid), approved, signedRow).isEmpty());
        QVERIFY(!signerChanged(verified(false, false, true), approved, signedRow).isEmpty());
        QVERIFY(!signerChanged(verified(true, false, true, kOtherDid), approved, signedRow).isEmpty());
        QVERIFY(!signerChanged(verified(true, true, false, kOtherDid), approved, signedRow).isEmpty());
        QVERIFY(!signerChanged({}, approved, signedRow).isEmpty());   // the check never ran
        QCOMPARE(signerChanged(verified(false, false, true), approved, looseRow), QString());
        QVERIFY(!signerChanged(verified(true, true, true, kOtherDid), approved, looseRow).isEmpty());
    }

    // A request is for a local file when it names one, not when it has no
    // repository: a catalogue release without one used to be checked as a
    // file that isn't there, and could be approved unchecked. A file Basecamp
    // can't verify can't be installed.
    void aRequestIsALocalFileWhenItNamesOne()
    {
        QVERIFY(!requestsLocalFile({{QStringLiteral("name"), QStringLiteral("app")},
                                    {QStringLiteral("repositoryUrl"), QString()}}));
        QVERIFY(!requestsLocalFile({{QStringLiteral("localPath"), QString()},
                                    {QStringLiteral("repositoryUrl"), QStringLiteral("https://repo.example")}}));
        QVERIFY(requestsLocalFile({{QStringLiteral("localPath"), QStringLiteral("/home/u/app.lgx")}}));
        QCOMPARE(fromVerifiedFile(QStringLiteral("app"), {}, {}, {}).status, Status::Invalid);
    }

    // A reinstall fetches the package anew, so it is judged by the release
    // fetched, and approved signed by its key: judged by the copy on disk,
    // it was approved unsigned, and a signed package could never be
    // reinstalled.
    void aReinstallIsJudgedByWhatItFetches()
    {
        const QVariantList resolved{planRow(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"))};
        const QList<Package> plan = judgePlan(
            QStringLiteral("app"), resolved,
            [](const QVariantMap&) { return true; },
            [](const QVariantMap& row) {
                return QVariantList{release(row.value(QStringLiteral("name")).toString(), row.value(QStringLiteral("version")).toString(),
                                            row.value(QStringLiteral("rootHash")).toString(), kOtherDid)};
            },
            [](const QVariantMap&) {
                return QVariantMap{{QStringLiteral("installType"), QStringLiteral("user")},
                                   {QStringLiteral("signerDid"), kOtherDid}};
            },
            {kOtherDid});
        QCOMPARE(plan.size(), 1);
        QVERIFY(!plan[0].installed);
        QCOMPARE(plan[0].status, Status::Trusted);
        QCOMPARE(approvedPlan(QStringLiteral("app"), resolved, plan).first().toMap()
                     .value(QStringLiteral("signer")).toString(), kOtherDid);
    }

    // A package named like one Basecamp ships replaces part of Basecamp, so
    // only Logos may sign it, whoever else is trusted: it used to install
    // without asking when any trusted key signed it.
    void whatShipsWithBasecampIsLogossOnly()
    {
        const QStringList bundled{QStringLiteral("capability_module")};
        const QVariantMap bundledCopy{{QStringLiteral("installType"), QStringLiteral("embedded")}};
        const auto replacing = [&](const QString& did) {
            return asReplacement({QStringLiteral("capability_module"), QStringLiteral("9.0.0"),
                                  did.isEmpty() ? Status::Unsigned : Status::Trusted, did},
                                 bundledCopy, bundled);
        };
        QCOMPARE(replacing(kOtherDid).status, Status::Invalid);
        QVERIFY(replacing(kOtherDid).reserved);
        QCOMPARE(replacing(QString()).status, Status::Invalid);
        QCOMPARE(replacing(logosDid()).status, Status::Trusted);
        QVERIFY(!replacing(logosDid()).reserved);
        // Another name is anyone's.
        QCOMPARE(asReplacement({QStringLiteral("chat"), {}, Status::Trusted, kOtherDid}, {}, bundled).status,
                 Status::Trusted);

        const QVariantMap s = summary({replacing(kOtherDid)}, {}, QStringLiteral("capability_module"));
        QCOMPARE(s.value(QStringLiteral("blocked")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("packages")).toList().first().toMap()
                     .value(QStringLiteral("reserved")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("byName")).toMap().value(QStringLiteral("capability_module")).toMap()
                     .value(QStringLiteral("reserved")).toBool(), true);

        // In a plan as well, trusted signer or not.
        const QList<Package> plan = judgePlan(
            QStringLiteral("chat"),
            {planRow(QStringLiteral("chat"), QStringLiteral("1.0.0"), QStringLiteral("a")),
             planRow(QStringLiteral("capability_module"), QStringLiteral("9.0.0"), QStringLiteral("c"))},
            [](const QVariantMap&) { return false; },
            [](const QVariantMap& row) {
                return QVariantList{release(row.value(QStringLiteral("name")).toString(), row.value(QStringLiteral("version")).toString(),
                                            row.value(QStringLiteral("rootHash")).toString(), kOtherDid)};
            },
            [](const QVariantMap&) { return QVariantMap{}; }, {kOtherDid}, bundled);
        QCOMPARE(plan[0].status, Status::Trusted);
        QCOMPARE(plan[1].status, Status::Invalid);
    }

    // A record is found by its exact name before one differing only in case,
    // which a case-insensitive file system finds too: the first
    // case-insensitive match used to stand for an exact one listed after it.
    void anInstalledRecordIsFoundByItsExactNameFirst()
    {
        const QVariantList installed{
            QVariantMap{{QStringLiteral("name"), QStringLiteral("Wallet_UI")},
                        {QStringLiteral("signerDid"), kOtherDid}},
            QVariantMap{{QStringLiteral("name"), QStringLiteral("wallet_ui")},
                        {QStringLiteral("signerDid"), kThirdDid}}};
        QCOMPARE(installedRecordOf(installed, QStringLiteral("wallet_ui"))
                     .value(QStringLiteral("signerDid")).toString(), kThirdDid);
        QCOMPARE(installedRecordOf(installed, QStringLiteral("WALLET_ui"))
                     .value(QStringLiteral("signerDid")).toString(), kOtherDid);
        QVERIFY(installedRecordOf(installed, QStringLiteral("chat")).isEmpty());
    }

    // The names the runtime trusts as callers are Logos's, like what Basecamp
    // ships: a package named core_service used to install from any trusted
    // publisher, and package_manager let it call.
    void callerNamesAreReservedLikeWhatShips()
    {
        const QStringList reserved = reservedNames({QStringLiteral("capability_module")},
                                                   {QStringLiteral("wallet_ui")});
        for (const char* name : {"capability_module", "wallet_ui", "wallet_ui_backend", "core_service"})
            QVERIFY2(reserved.contains(QLatin1String(name)), name);
        // Whatever Basecamp ships: each name that may call package_manager.
        QVERIFY(packageModuleCallers().contains(QStringLiteral("core")));
        for (const QString& caller : packageModuleCallers())
            QVERIFY2(reservedNames({}, {}).contains(caller), qPrintable(caller));
        const Package daemon = asReplacement({QStringLiteral("Core_Service"), QStringLiteral("1.0.0"),
                                              Status::Trusted, kOtherDid}, {}, reserved);
        QCOMPARE(daemon.status, Status::Invalid);
        QVERIFY(daemon.reserved);
        QCOMPARE(asReplacement({QStringLiteral("core"), QStringLiteral("1.0.0"), Status::Trusted, logosDid()},
                               {}, reserved).status,
                 Status::Trusted);
    }

    // An update signed by another publisher than the copy it replaces asks,
    // says so, and is not offered for trust, even when its signer is
    // trusted: it used to install without a word.
    void anUpdateSignedBySomeoneElseAsks()
    {
        const QVariantMap had{{QStringLiteral("installType"), QStringLiteral("user")},
                              {QStringLiteral("signerDid"), kThirdDid}};
        const QVariantList keys{QVariantMap{{QStringLiteral("did"), kOtherDid},
                                            {QStringLiteral("displayName"), QStringLiteral("Other")}},
                                QVariantMap{{QStringLiteral("did"), kThirdDid},
                                            {QStringLiteral("displayName"), QStringLiteral("Third")}}};
        const Package trustedUpdate = asReplacement(
            {QStringLiteral("app"), QStringLiteral("2.0.0"), Status::Trusted, kOtherDid, QStringLiteral("Other")},
            had, {});
        QCOMPARE(trustedUpdate.replacesSignedBy, kThirdDid);
        QVariantMap s = summary({trustedUpdate}, keys, QStringLiteral("app"));
        QCOMPARE(s.value(QStringLiteral("needsPrompt")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("canTrust")).toBool(), false);
        QCOMPARE(s.value(QStringLiteral("trustCoversAll")).toBool(), false);
        QCOMPARE(s.value(QStringLiteral("byName")).toMap().value(QStringLiteral("app")).toMap()
                     .value(QStringLiteral("replaces")).toString(), QStringLiteral("Third"));
        QCOMPARE(s.value(QStringLiteral("packages")).toList().first().toMap()
                     .value(QStringLiteral("replaces")).toString(), QStringLiteral("Third"));

        const Package untrustedUpdate = asReplacement(
            {QStringLiteral("app"), QStringLiteral("2.0.0"), Status::Untrusted, kOtherDid, QStringLiteral("Other")},
            had, {});
        QCOMPARE(summary({untrustedUpdate}, {}, QStringLiteral("app")).value(QStringLiteral("canTrust")).toBool(), false);

        // The same key, or Logos with another of its keys: nothing changes hands.
        QVERIFY(asReplacement({QStringLiteral("app"), {}, Status::Trusted, kThirdDid}, had, {}).replacesSignedBy.isEmpty());
        const LogosKeys rotated{{kOtherDid, logosDid()}, {}};
        QVERIFY(asReplacement({QStringLiteral("app"), {}, Status::Trusted, kOtherDid},
                              {{QStringLiteral("installType"), QStringLiteral("user")},
                               {QStringLiteral("signerDid"), logosDid()}}, {}, rotated)
                    .replacesSignedBy.isEmpty());
        // Matched as a case-insensitive file system matches it.
        QCOMPARE(asReplacement({QStringLiteral("Capability_Module"), {}, Status::Trusted, kOtherDid}, {},
                               {QStringLiteral("capability_module")}).status, Status::Invalid);
    }

    // A copy nobody signed can't be told to come from the publisher of what
    // replaces it, so only Logos takes its place unasked: a trusted key used to
    // replace another's unsigned app without a word.
    void replacingACopyNobodySignedAsks()
    {
        const QVariantMap unsignedCopy{{QStringLiteral("installType"), QStringLiteral("user")}};
        const Package other = asReplacement(
            {QStringLiteral("app"), QStringLiteral("2.0.0"), Status::Trusted, kOtherDid, QStringLiteral("Other")},
            unsignedCopy, {});
        QVERIFY(other.replacesUnverified);
        const QVariantMap s = summary({other}, {QVariantMap{{QStringLiteral("did"), kOtherDid},
                                                            {QStringLiteral("displayName"), QStringLiteral("Other")}}},
                                      QStringLiteral("app"));
        QCOMPARE(s.value(QStringLiteral("needsPrompt")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("canTrust")).toBool(), false);
        QCOMPARE(s.value(QStringLiteral("byName")).toMap().value(QStringLiteral("app")).toMap()
                     .value(QStringLiteral("replacesUnverified")).toBool(), true);
        QVERIFY(!asReplacement({QStringLiteral("app"), {}, Status::Trusted, logosDid()}, unsignedCopy, {})
                     .replacesUnverified);
    }

    // A release installs under its own manifest's name, which the download is
    // held to: one listed as "cool_backend" but installing as
    // capability_module used to pass every check, and so did one with no
    // manifest at all.
    void aReleaseInstallsUnderTheNameItIsListedAs()
    {
        QCOMPARE(fromCatalog(QStringLiteral("cool_backend"), QStringLiteral("1.0.0"), QStringLiteral("a"),
                             {release(QStringLiteral("capability_module"), QStringLiteral("1.0.0"),
                                      QStringLiteral("a"), kOtherDid)},
                             {kOtherDid}).status, Status::Invalid);
        QVariantMap nameless = release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"), kOtherDid);
        nameless.remove(QStringLiteral("manifest"));
        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"), {nameless},
                             {kOtherDid}).status, Status::Invalid);
        QCOMPARE(fromCatalog(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"),
                             {release(QStringLiteral("app"), QStringLiteral("1.0.0"), QStringLiteral("a"), kOtherDid)},
                             {kOtherDid}).status, Status::Trusted);
    }

    // An address claiming Logos isn't shown for another key; a path may
    // mention anything.
    void anAddressClaimingLogosIsLogoss()
    {
        QCOMPARE(publisherUrl(QStringLiteral("https://logos.co"), kOtherDid), QString());
        QCOMPARE(publisherUrl(QStringLiteral("logos.co"), kOtherDid), QString());
        QCOMPARE(publisherUrl(QStringLiteral("https://logos-official.example"), kOtherDid), QString());
        QCOMPARE(publisherUrl(QStringLiteral("https://github.com/acme/logos-tools"), kOtherDid),
                 QStringLiteral("https://github.com/acme/logos-tools"));
        QCOMPARE(publisherUrl(QStringLiteral("https://acme.example"), kOtherDid), QStringLiteral("https://acme.example"));
        QCOMPARE(publisherUrl(QStringLiteral("https://elsewhere.example"), logosDid()), logosUrl());
    }

    // A word of a name that starts reading as Logos makes it Logos's, not
    // just the whole name: "Logos Official" used to be offered for trust.
    void aNameWithLogosInItIsLogoss()
    {
        const auto flagged = [](const QString& claimed, const QString& did) {
            const QVariantMap s = summary({{QStringLiteral("app"), {}, Status::Untrusted, did, claimed}}, {},
                                          QStringLiteral("app"));
            return s.value(QStringLiteral("byName")).toMap().value(QStringLiteral("app")).toMap()
                .value(QStringLiteral("impersonates")).toString();
        };
        for (const QString& name : {QStringLiteral("Logos Official"), QStringLiteral("Official Logos"),
                                    QStringLiteral("LogosHub"), QStringLiteral("My L o g o s"),
                                    QStringLiteral("the-logos-team"), QStringLiteral("L0gos Labs")})
            QVERIFY2(flagged(name, kOtherDid) == QStringLiteral("Logos"), qPrintable(name));
        for (const QString& name : {QStringLiteral("Neologos"), QStringLiteral("Apologos Labs"),
                                    QStringLiteral("Other")})
            QVERIFY2(flagged(name, kOtherDid).isEmpty(), qPrintable(name));
        QCOMPARE(flagged(QStringLiteral("Logos Official"), logosDid()), QString());   // Logos's own key
        QCOMPARE(publisherName(QStringLiteral("Logos Official"), kOtherDid), QString());
    }

    void theTrustedDidsComeFromTheKeyring()
    {
        QCOMPARE(trustedDids({QVariantMap{{QStringLiteral("name"), QStringLiteral("logos")},
                                          {QStringLiteral("did"), logosDid()}},
                              QVariantMap{{QStringLiteral("name"), QStringLiteral("again")},
                                          {QStringLiteral("did"), logosDid()}},
                              QVariantMap{{QStringLiteral("name"), QStringLiteral("empty")}}}),
                 QStringList{logosDid()});
    }

};

QTEST_GUILESS_MAIN(InstallTrustTest)
#include "install_trust_test.moc"
