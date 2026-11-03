// srcdeps: utils/PublisherKeyring.cpp utils/InstallTrust.cpp
// libdeps: ICU::i18n ICU::uc
//
// The trusted publishers (app/utils/PublisherKeyring.{h,cpp}) against a fake
// of package_manager's keyring calls that answers only when told to:
// trusting waits for the keyring, only a new keyring gets Logos, and a
// change that fails is reported and leaves the publisher as it was.

#include "../app/utils/InstallTrust.h"
#include "../app/utils/PublisherKeyring.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace logos::trust;

namespace {

// Canonical did:jwk of two made-up Ed25519 keys (see install_trust_test).
const QString kAcmeDid = QStringLiteral(
    "did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IkFRSURCQVVHQndnSkNnc01EUTRQRUJFU0V4"
    "UVZGaGNZR1JvYkhCMGVIeUEifQ");
const QString kZetaDid = QStringLiteral(
    "did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlpXWm5hR2xxYTJ4dGJtOXdjWEp6ZEhWMmQz"
    "aDVlbnQ4Zlg1X2dJR0NnNFEifQ");

// package_manager's keyring, as listTrustedKeys / addTrustedKey /
// removeTrustedKey keep it. Like liblgx, any call makes `folder`. Each call is
// answered when told to, unless `deferred` is off; a name in `failing` cannot
// be changed, and with `listFails` the keyring cannot be read.
struct FakeKeyring {
    QString folder;
    QVariantList keys;   // {name, did, displayName, url, addedAt}
    QStringList calls;
    QStringList failing;
    bool listFails = false;
    bool deferred = true;
    QList<std::function<void()>> owed;

    PublisherKeyring::Keyring seam()
    {
        PublisherKeyring::Keyring k;
        k.list = [this](std::function<void(bool, const QVariantList&)> then) {
            called(QStringLiteral("list"));
            answer([this, then]() { then(!listFails, listFails ? QVariantList{} : keys); });
        };
        k.add = [this](const QVariantMap& key, PublisherKeyring::Done done) {
            const QString name = key.value(QStringLiteral("name")).toString();
            called(QStringLiteral("add:") + name);
            answer([this, key, name, done]() {
                if (failing.contains(name)) { done(false, QStringLiteral("disk full")); return; }
                // As liblgx: the key file of that name is written anew.
                for (int i = 0; i < keys.size(); ++i)
                    if (keys.at(i).toMap().value(QStringLiteral("name")).toString() == name)
                        keys.removeAt(i--);
                QVariantMap added = key;
                added[QStringLiteral("addedAt")] = QStringLiteral("2026-11-03T10:00:00Z");
                keys << added;
                done(true, {});
            });
        };
        k.remove = [this](const QString& name, PublisherKeyring::Done done) {
            called(QStringLiteral("remove:") + name);
            answer([this, name, done]() {
                if (failing.contains(name)) { done(false, QStringLiteral("disk full")); return; }
                for (int i = 0; i < keys.size(); ++i) {
                    if (keys.at(i).toMap().value(QStringLiteral("name")).toString() != name) continue;
                    keys.removeAt(i);
                    done(true, {});
                    return;
                }
                done(false, QStringLiteral("Key not found: ") + name);
            });
        };
        return k;
    }

    void called(const QString& call)
    {
        calls << call;
        if (!folder.isEmpty()) QDir().mkpath(folder);
    }

    void answer(std::function<void()> a)
    {
        if (deferred) owed << a;
        else a();
    }

    void answerNext()
    {
        if (!owed.isEmpty()) owed.takeFirst()();
    }

    // Every answer owed, including those owed for the answers given.
    void settle()
    {
        while (!owed.isEmpty())
            owed.takeFirst()();
    }
};

QVariantMap publisher(const QString& did, const QString& name)
{
    return {{QStringLiteral("did"), did}, {QStringLiteral("name"), name},
            {QStringLiteral("url"), QStringLiteral("https://example.org")}};
}

QVariantMap key(const QString& name, const QString& did, const QString& displayName)
{
    return {{QStringLiteral("name"), name}, {QStringLiteral("did"), did},
            {QStringLiteral("displayName"), displayName}, {QStringLiteral("url"), QString()},
            {QStringLiteral("addedAt"), QStringLiteral("2026-10-01T10:00:00Z")}};
}

QVariantMap rowNamed(const QVariantList& rows, const QString& name)
{
    for (const QVariant& v : rows)
        if (v.toMap().value(QStringLiteral("name")).toString() == name)
            return v.toMap();
    return {};
}

} // namespace

class PublisherKeyringTest : public QObject
{
    Q_OBJECT

private slots:
    // "Trust & install" goes on only once the keyring has the key, and has
    // been read back: it used to answer at once while the key was still
    // being added, whether or not it ever was.
    void trustingWaitsForTheKeyring()
    {
        FakeKeyring fake;
        PublisherKeyring keyring(fake.seam());
        bool answered = false;
        QString failure = QStringLiteral("unset");
        keyring.trust(publisher(kAcmeDid, QStringLiteral("Acme")), [&](const QString& error) {
            answered = true;
            failure = error;
        });
        QVERIFY(!answered);
        fake.settle();
        QVERIFY(answered);
        QCOMPARE(failure, QString());
        QCOMPARE(trustedDids(keyring.keys()), QStringList{kAcmeDid});
        QCOMPARE(fake.calls, (QStringList{QStringLiteral("add:") + keyNameFor(kAcmeDid),
                                          QStringLiteral("list")}));
    }

    // A key the keyring will not take stops the install and says why.
    void aKeyTheKeyringRefusesStopsTheInstall()
    {
        FakeKeyring fake;
        fake.failing << keyNameFor(kAcmeDid);
        PublisherKeyring keyring(fake.seam());
        QString failure;
        keyring.trust(publisher(kAcmeDid, QStringLiteral("Acme")),
                      [&](const QString& error) { failure = error; });
        fake.settle();
        QCOMPARE(failure, QStringLiteral("Couldn't trust “Acme”: disk full"));
        QVERIFY(keyring.keys().isEmpty());
    }

    // Logos goes in under Basecamp's own name for it, whatever a package
    // called it.
    void trustingLogosUsesBasecampsNameForIt()
    {
        FakeKeyring fake;
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam());
        keyring.trust(publisher(logosDid(), QStringLiteral("Logos Inc")), [](const QString&) {});
        QCOMPARE(fake.keys.size(), 1);
        const QVariantMap added = fake.keys.first().toMap();
        QCOMPARE(added.value(QStringLiteral("name")).toString(), logosKeyName());
        QCOMPARE(added.value(QStringLiteral("displayName")).toString(), logosName());
    }

    // A new keyring (no folder yet) gets Logos. One Basecamp has used before,
    // where the first call made the folder, is left as it is: a Logos the
    // user deleted stays deleted.
    void onlyANewKeyringGetsLogos()
    {
        QTemporaryDir home;
        FakeKeyring fake;
        fake.folder = home.filePath(QStringLiteral("keyring"));
        fake.deferred = false;
        {
            PublisherKeyring keyring(fake.seam());
            keyring.configure(fake.folder);
            QCOMPARE(fake.calls, (QStringList{QStringLiteral("add:") + logosKeyName(),
                                              QStringLiteral("list")}));
            QCOMPARE(trustedDids(keyring.keys()), QStringList{logosDid()});
            keyring.remove(logosKeyName());
            QVERIFY(keyring.keys().isEmpty());
        }
        fake.calls.clear();
        PublisherKeyring restarted(fake.seam());
        restarted.configure(fake.folder);
        QCOMPARE(fake.calls, QStringList{QStringLiteral("list")});
        QVERIFY(restarted.keys().isEmpty());
    }

    // Adding Logos to a new keyring is tried again on each read until a read
    // shows it there: one failed first try, after which the folder exists,
    // used to leave the user without Logos for good. It stops then, so a
    // Logos deleted afterwards stays deleted.
    void aNewKeyringKeepsTryingForLogos()
    {
        QTemporaryDir home;
        FakeKeyring fake;
        fake.folder = home.filePath(QStringLiteral("keyring"));
        fake.deferred = false;
        fake.failing << logosKeyName();
        PublisherKeyring keyring(fake.seam());
        keyring.configure(fake.folder);
        QVERIFY(keyring.keys().isEmpty());

        fake.failing.clear();
        keyring.refresh();
        QCOMPARE(trustedDids(keyring.keys()), QStringList{logosDid()});

        keyring.remove(logosKeyName());
        fake.calls.clear();
        keyring.refresh();
        QCOMPARE(fake.calls, QStringList{QStringLiteral("list")});
        QVERIFY(keyring.keys().isEmpty());
    }

    // After Logos rotates its key, a keyring holding the old one is left as
    // it is: it stands for Logos, whose row shows the new key, and trusting
    // Logos again adds the new key. A new keyring gets the new key.
    void anOldLogosKeyStillStandsForLogos()
    {
        const LogosKeys rotated{{kZetaDid, logosDid()}, {}};
        QTemporaryDir home;
        FakeKeyring fake;
        fake.folder = home.filePath(QStringLiteral("keyring"));
        QDir().mkpath(fake.folder);
        fake.keys << key(logosKeyName(), logosDid(), logosName());
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam(), nullptr, rotated);
        keyring.configure(fake.folder);
        QCOMPARE(fake.calls, QStringList{QStringLiteral("list")});
        const QVariantMap row = keyring.publishers().first().toMap();
        QCOMPARE(row.value(QStringLiteral("builtIn")).toBool(), true);
        QCOMPARE(row.value(QStringLiteral("did")).toString(), kZetaDid);
        QCOMPARE(row.value(QStringLiteral("fingerprint")).toString(), fingerprint(kZetaDid));
        QCOMPARE(trustedDids(keyring.keys(), rotated), (QStringList{kZetaDid, logosDid()}));

        keyring.remove(logosKeyName());
        QVERIFY(trustedDids(keyring.keys(), rotated).isEmpty());
        keyring.trust(publisher(logosDid(), QStringLiteral("Logos")), [](const QString&) {});
        QCOMPARE(fake.keys.first().toMap().value(QStringLiteral("did")).toString(), kZetaDid);

        QTemporaryDir other;
        FakeKeyring fresh;
        fresh.folder = other.filePath(QStringLiteral("keyring"));
        fresh.deferred = false;
        PublisherKeyring newKeyring(fresh.seam(), nullptr, rotated);
        newKeyring.configure(fresh.folder);
        QCOMPARE(fresh.keys.first().toMap().value(QStringLiteral("did")).toString(), kZetaDid);
    }

    // A row per key file: Logos first, under Basecamp's name and address
    // whatever its file says, then the others by name.
    void publishersListLogosFirstUnderBasecampsName()
    {
        FakeKeyring fake;
        fake.keys << key(QStringLiteral("zeta"), kZetaDid, QStringLiteral("Zeta"))
                  << key(QStringLiteral("logos-official"), logosDid(), QStringLiteral("Logos Inc"))
                  << key(QStringLiteral("acme"), kAcmeDid, QStringLiteral("acme"))
                  << key(QStringLiteral("acme-again"), kAcmeDid, QStringLiteral("acme"));
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam());
        keyring.refresh();
        const QVariantList rows = keyring.publishers();
        QCOMPARE(rows.size(), 4);
        const QVariantMap logos = rows.at(0).toMap();
        QCOMPARE(logos.value(QStringLiteral("name")).toString(), QStringLiteral("logos-official"));
        QCOMPARE(logos.value(QStringLiteral("displayName")).toString(), logosName());
        QCOMPARE(logos.value(QStringLiteral("url")).toString(), logosUrl());
        QCOMPARE(logos.value(QStringLiteral("builtIn")).toBool(), true);
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("acme"));
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("fingerprint")).toString(), fingerprint(kAcmeDid));
        QCOMPARE(rows.at(2).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("acme-again"));
        QCOMPARE(rows.at(3).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("zeta"));
    }

    // Logos can be deleted like any other publisher.
    void logosCanBeDeleted()
    {
        FakeKeyring fake;
        fake.keys << key(logosKeyName(), logosDid(), logosName());
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam());
        keyring.refresh();
        keyring.remove(logosKeyName());
        QVERIFY(keyring.keys().isEmpty());
        QVERIFY(keyring.publishers().isEmpty());
    }

    // A publisher being deleted takes no other delete, and its row stays busy
    // until the keyring is read back, so it never shows the old state.
    void aRowIsBusyUntilTheKeyringIsReadBack()
    {
        FakeKeyring fake;
        fake.keys << key(QStringLiteral("acme"), kAcmeDid, QStringLiteral("Acme"));
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam());
        keyring.refresh();
        fake.deferred = true;
        fake.calls.clear();

        keyring.remove(QStringLiteral("acme"));
        keyring.remove(QStringLiteral("acme"));
        QCOMPARE(fake.calls, QStringList{QStringLiteral("remove:acme")});
        fake.answerNext();   // the delete
        QCOMPARE(rowNamed(keyring.publishers(), QStringLiteral("acme")).value(QStringLiteral("busy")).toBool(),
                 true);
        fake.answerNext();   // the read
        QVERIFY(keyring.publishers().isEmpty());
        QCOMPARE(fake.calls, (QStringList{QStringLiteral("remove:acme"), QStringLiteral("list")}));
    }

    // A delete the keyring refuses is reported, and the publisher stays.
    void aDeleteTheKeyringRefusesIsReported()
    {
        FakeKeyring fake;
        fake.keys << key(QStringLiteral("acme"), kAcmeDid, QStringLiteral("Acme"));
        fake.failing << QStringLiteral("acme");
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam());
        keyring.refresh();
        QSignalSpy failed(&keyring, &PublisherKeyring::changeFailed);

        keyring.remove(QStringLiteral("acme"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("acme"));
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("Couldn't delete “Acme”: disk full"));
        QCOMPARE(rowNamed(keyring.publishers(), QStringLiteral("acme")).value(QStringLiteral("busy")).toBool(),
                 false);
        QCOMPARE(trustedDids(keyring.keys()), QStringList{kAcmeDid});
    }

    // A key already gone when it is deleted (another tool took it) is
    // deleted: it used to be reported as a delete that failed.
    void aKeyAlreadyGoneIsDeleted()
    {
        FakeKeyring fake;
        fake.keys << key(QStringLiteral("acme"), kAcmeDid, QStringLiteral("Acme"));
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam());
        keyring.refresh();
        fake.keys.clear();
        QSignalSpy failed(&keyring, &PublisherKeyring::changeFailed);

        keyring.remove(QStringLiteral("acme"));
        QCOMPARE(failed.count(), 0);
        QVERIFY(keyring.publishers().isEmpty());
    }

    // package_manager lists only the key files it can read. Fewer keys than
    // files is a keyring that can't be read either: it used to look like one
    // that trusts fewer publishers, freeing the others' names.
    void aKeyringReadInPartTrustsNobody()
    {
        QTemporaryDir home;
        FakeKeyring fake;
        fake.folder = home.filePath(QStringLiteral("keyring"));
        QDir().mkpath(fake.folder);
        for (const QString& file : {QStringLiteral("acme.json"), QStringLiteral("zeta.json")}) {
            QFile f(fake.folder + QLatin1Char('/') + file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{}");
        }
        fake.keys << key(QStringLiteral("acme"), kAcmeDid, QStringLiteral("Acme"));
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam());
        QSignalSpy failed(&keyring, &PublisherKeyring::changeFailed);
        keyring.configure(fake.folder);
        QVERIFY(!keyring.readable());
        QVERIFY(keyring.keys().isEmpty());
        QCOMPARE(failed.count(), 1);

        fake.keys << key(QStringLiteral("zeta"), kZetaDid, QStringLiteral("Zeta"));
        keyring.refresh();
        QVERIFY(keyring.readable());
        QCOMPARE(keyring.keys().size(), 2);
    }

    // A list with no key to sign with (only possible if a release gets it
    // wrong) adds nothing and breaks nothing: it used to crash.
    void aListWithNoCurrentLogosKeyBreaksNothing()
    {
        const LogosKeys noneLeft{{}, {logosDid()}};
        QTemporaryDir home;
        FakeKeyring fake;
        fake.folder = home.filePath(QStringLiteral("keyring"));
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam(), nullptr, noneLeft);
        keyring.configure(fake.folder);
        QCOMPARE(fake.calls, QStringList{QStringLiteral("list")});

        fake.keys << key(logosKeyName(), logosDid(), logosName());
        keyring.refresh();
        const QVariantMap row = keyring.publishers().first().toMap();
        QCOMPARE(row.value(QStringLiteral("builtIn")).toBool(), true);
        QCOMPARE(row.value(QStringLiteral("did")).toString(), QString());
        QVERIFY(trustedDids(keyring.keys(), noneLeft).isEmpty());
    }

    // A key that isn't Logos's, put in the keyring as "Logos" by another
    // tool, is not shown as Logos: its row goes by its file name, or by the
    // key when the file name reads as Logos too (it used to fall back to it),
    // and an address claiming Logos isn't shown.
    void aKeyCalledLogosThatIsNotLogossIsNotShownAsLogos()
    {
        FakeKeyring fake;
        QVariantMap posing = key(QStringLiteral("fake"), kAcmeDid, QStringLiteral("Logos"));
        posing[QStringLiteral("url")] = QStringLiteral("https://logos.co");
        fake.keys << posing << key(QStringLiteral("Logos"), kZetaDid, QStringLiteral("Logos Official"));
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam());
        keyring.refresh();
        const QVariantMap byFile = rowNamed(keyring.publishers(), QStringLiteral("fake"));
        QCOMPARE(byFile.value(QStringLiteral("builtIn")).toBool(), false);
        QCOMPARE(byFile.value(QStringLiteral("displayName")).toString(), QStringLiteral("fake"));
        QCOMPARE(byFile.value(QStringLiteral("url")).toString(), QString());
        QCOMPARE(rowNamed(keyring.publishers(), QStringLiteral("Logos")).value(QStringLiteral("displayName")).toString(),
                 QStringLiteral("key ") + fingerprint(kZetaDid));
    }

    // A keyring that can't be read trusts nobody, as what was read before
    // may be out of date, and says so: a delete whose read-back failed used
    // to leave the publisher trusted.
    void aKeyringThatCannotBeReadTrustsNobody()
    {
        FakeKeyring fake;
        fake.keys << key(QStringLiteral("acme"), kAcmeDid, QStringLiteral("Acme"))
                  << key(QStringLiteral("zeta"), kZetaDid, QStringLiteral("Zeta"));
        fake.deferred = false;
        PublisherKeyring keyring(fake.seam());
        keyring.refresh();
        QCOMPARE(keyring.keys().size(), 2);
        QVERIFY(keyring.readable());
        QSignalSpy failed(&keyring, &PublisherKeyring::changeFailed);

        fake.listFails = true;
        keyring.remove(QStringLiteral("acme"));
        QCOMPARE(fake.keys.size(), 1);
        QVERIFY(!keyring.readable());
        QVERIFY(keyring.keys().isEmpty());
        QVERIFY(keyring.publishers().isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QString());

        fake.listFails = false;
        keyring.refresh();
        QVERIFY(keyring.readable());
        QCOMPARE(trustedDids(keyring.keys()), QStringList{kZetaDid});
    }
};

QTEST_GUILESS_MAIN(PublisherKeyringTest)
#include "publisher_keyring_test.moc"
