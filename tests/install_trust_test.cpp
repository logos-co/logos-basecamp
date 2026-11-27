// srcdeps: utils/InstallTrust.cpp
// libdeps: basecamp_installer_trust
//
// Unit tests for trust at install (app/utils/InstallTrust.{h,cpp}): what only
// Basecamp judges on top of basecamp-installer-common's Trust.h (its own apps
// and modules, an app's backend name, what an update replaces), how a plan is
// judged row by row, and the approval it answers with. The judge itself is
// tested there.

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

    // Basecamp's own apps come with it and update only with it: nothing
    // installs under their names, Logos-signed or not, in any case.
    void builtInAppsNeverInstall()
    {
        for (const QString& name : {QStringLiteral("package_manager_ui"),
                                    QStringLiteral("package_manager_ui_backend"),
                                    QStringLiteral("Package_Manager_UI")}) {
            for (const QString& did : {logosDid(), kOtherDid, QString()}) {
                const Package p = asReplacement({name, QStringLiteral("9.0.0"),
                                                 did.isEmpty() ? Status::Unsigned : Status::Trusted, did},
                                                {}, {});
                QCOMPARE(p.status, Status::Invalid);
                QVERIFY(p.builtIn);
                QVERIFY(!p.reserved);
            }
        }

        const Package logos = asReplacement({QStringLiteral("package_manager_ui"), QStringLiteral("9.0.0"),
                                             Status::Trusted, logosDid()}, {}, {});
        const QVariantMap s = summary({logos}, {}, QStringLiteral("package_manager_ui"));
        QCOMPARE(s.value(QStringLiteral("blocked")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("packages")).toList().first().toMap()
                     .value(QStringLiteral("builtIn")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("byName")).toMap().value(QStringLiteral("package_manager_ui")).toMap()
                     .value(QStringLiteral("builtIn")).toBool(), true);

        // As a dependency in a plan as well.
        const QList<Package> plan = judgePlan(
            QStringLiteral("chat"),
            {planRow(QStringLiteral("chat"), QStringLiteral("1.0.0"), QStringLiteral("a")),
             planRow(QStringLiteral("package_manager_ui"), QStringLiteral("9.0.0"), QStringLiteral("p"))},
            [](const QVariantMap&) { return false; },
            [](const QVariantMap& row) {
                return QVariantList{release(row.value(QStringLiteral("name")).toString(), row.value(QStringLiteral("version")).toString(),
                                            row.value(QStringLiteral("rootHash")).toString(), logosDid())};
            },
            [](const QVariantMap&) { return QVariantMap{}; }, {logosDid()});
        QCOMPARE(plan[0].status, Status::Trusted);
        QCOMPARE(plan[1].status, Status::Invalid);
        QVERIFY(plan[1].builtIn);
    }


    // The names the runtime trusts as callers are Logos's, like what Basecamp
    // ships: a package named core_service used to install from any trusted
    // publisher, and package_manager let it call.
    void callerNamesAreReservedLikeWhatShips()
    {
        const QStringList reserved = reservedNames({QStringLiteral("capability_module")},
                                                   {QStringLiteral("wallet_ui")});
        for (const char* name : {"capability_module", "wallet_ui", "wallet_ui_backend", "intents_module"})
            QVERIFY2(reserved.contains(QLatin1String(name)), name);
        // Whatever Basecamp ships: each name that may call package_manager.
        QVERIFY(packageModuleCallers().contains(QStringLiteral("core")));
        for (const QString& caller : packageModuleCallers())
            QVERIFY2(reservedNames({}, {}).contains(caller), qPrintable(caller));
        const Package broker = asReplacement({QStringLiteral("Intents_Module"), QStringLiteral("1.0.0"),
                                              Status::Trusted, kOtherDid}, {}, reserved);
        QCOMPARE(broker.status, Status::Invalid);
        QVERIFY(broker.reserved);
        QCOMPARE(asReplacement({QStringLiteral("intents_module"), QStringLiteral("1.0.0"), Status::Trusted,
                                logosDid()}, {}, reserved).status,
                 Status::Trusted);
    }

    // Logos Storage comes with Basecamp, yet the person may update it from any
    // publisher, as any package: it was Logos's only, so installing the
    // Storage app, which brings the catalogue's build, was refused.
    void storageUpdatesLikeAnyPackage()
    {
        const QStringList reserved = reservedNames(
            {QStringLiteral("storage_module"), QStringLiteral("package_manager"),
             QStringLiteral("capability_module")}, {});
        QVERIFY(!reserved.contains(QStringLiteral("storage_module"), Qt::CaseInsensitive));
        for (const char* name : {"package_manager", "capability_module", "intents_module"})
            QVERIFY2(reserved.contains(QLatin1String(name)), name);

        const QVariantMap bundledCopy{{QStringLiteral("installType"), QStringLiteral("embedded")}};
        const Package unsignedStorage = asReplacement(
            {QStringLiteral("storage_module"), QStringLiteral("3.0.2"), Status::Unsigned, {}},
            bundledCopy, reserved);
        QVERIFY(!unsignedStorage.reserved);
        QCOMPARE(unsignedStorage.status, Status::Unsigned);
        QCOMPARE(summary({unsignedStorage}, {}, QStringLiteral("storage_module"))
                     .value(QStringLiteral("blocked")).toBool(), false);

        const QList<Package> plan = judgePlan(
            QStringLiteral("storage_ui"),
            {planRow(QStringLiteral("storage_ui"), QStringLiteral("3.0.2"), QStringLiteral("a")),
             planRow(QStringLiteral("storage_module"), QStringLiteral("3.0.2"), QStringLiteral("s"))},
            [](const QVariantMap&) { return false; },
            [](const QVariantMap& row) {
                return QVariantList{release(row.value(QStringLiteral("name")).toString(),
                                            row.value(QStringLiteral("version")).toString(),
                                            row.value(QStringLiteral("rootHash")).toString(), kOtherDid)};
            },
            [](const QVariantMap&) { return QVariantMap{}; }, {kOtherDid}, reserved);
        QCOMPARE(plan[1].name, QStringLiteral("storage_module"));
        QVERIFY(!plan[1].reserved);
        QCOMPARE(plan[1].status, Status::Trusted);
    }

    // The names Basecamp and the runtime call modules under: a package under
    // one would act as them, so none installs, whoever signed it. A main_ui
    // Logos signed would still be the shell to every module.
    void identityNamesNeverInstall()
    {
        for (const char* name : {"core", "core_service", "Core_Service", "main_ui", "logos_link"}) {
            for (const QString& did : {logosDid(), kOtherDid}) {
                const Package p = asReplacement({QLatin1String(name), QStringLiteral("1.0.0"),
                                                 Status::Trusted, did}, {}, reservedNames({}, {}));
                QCOMPARE(p.status, Status::Invalid);
                QVERIFY2(p.builtIn, name);
            }
        }
    }

    // An app's backend ships inside the app's package: one installed under that
    // name, while the app is installed or installs with it, would make the
    // backend unregistrable. Only Logos may. A name that only ends in _backend,
    // with no such app, is a package like any other.
    void anAppsBackendNameIsReservedWhileTheAppIsThere()
    {
        const QVariantList installed{
            QVariantMap{{QStringLiteral("name"), QStringLiteral("chat_ui")}, {QStringLiteral("type"), QStringLiteral("ui_qml")}},
            QVariantMap{{QStringLiteral("name"), QStringLiteral("eth_rpc_module")}, {QStringLiteral("type"), QStringLiteral("core")}}};
        QVariantMap planned = planRow(QStringLiteral("wallet_ui"), QStringLiteral("1.0.0"), QStringLiteral("a"));
        planned[QStringLiteral("type")] = QStringLiteral("ui_qml");
        const QStringList reserved = reservedFor({}, installed, {planned});

        for (const char* name : {"chat_ui_backend", "Chat_UI_Backend", "wallet_ui_backend"}) {
            const Package p = asReplacement({QLatin1String(name), QStringLiteral("1.0.0"),
                                             Status::Trusted, kOtherDid}, {}, reserved);
            QCOMPARE(p.status, Status::Invalid);
            QVERIFY2(p.reserved, name);
        }
        QCOMPARE(asReplacement({QStringLiteral("chat_ui_backend"), QStringLiteral("1.0.0"),
                                Status::Trusted, logosDid()}, {}, reserved).status,
                 Status::Trusted);
        for (const char* name : {"eth_wallet_backend", "eth_rpc_module_backend", "chat_ui", "backend"}) {
            const Package p = asReplacement({QLatin1String(name), QStringLiteral("1.0.0"),
                                             Status::Trusted, kOtherDid}, {}, reserved);
            QVERIFY2(!p.reserved, name);
            QCOMPARE(p.status, Status::Trusted);
        }
    }

    // The Ethereum wallet brings eth_wallet_backend, a core module that only
    // ends in _backend: it installs with the app. A package in the same plan
    // under the app's own backend name does not.
    void aModuleThatOnlyEndsInBackendInstallsWithItsApp()
    {
        const auto onDisk = [](const QVariantMap&) { return false; };
        const auto versionsOf = [](const QVariantMap& row) {
            return QVariantList{release(row.value(QStringLiteral("name")).toString(),
                                        row.value(QStringLiteral("version")).toString(),
                                        row.value(QStringLiteral("rootHash")).toString(), kOtherDid)};
        };
        const auto installedOf = [](const QVariantMap&) { return QVariantMap{}; };
        const auto typed = [](const char* name, const char* type, const char* hash) {
            QVariantMap row = planRow(QLatin1String(name), QStringLiteral("0.1.0"), QLatin1String(hash));
            row[QStringLiteral("type")] = QLatin1String(type);
            return row;
        };
        const QVariantList wallet{typed("eth_wallet_ui", "ui_qml", "a"), typed("eth_wallet_backend", "core", "b")};
        for (const Package& p : judgePlan(QStringLiteral("eth_wallet_ui"), wallet, onDisk, versionsOf, installedOf,
                                          {kOtherDid}, reservedFor({}, {}, wallet))) {
            QVERIFY2(!p.reserved, qPrintable(p.name));
            QCOMPARE(p.status, Status::Trusted);
        }

        QVariantList impostor = wallet;
        impostor.append(typed("eth_wallet_ui_backend", "core", "c"));
        const QList<Package> judged = judgePlan(QStringLiteral("eth_wallet_ui"), impostor, onDisk, versionsOf,
                                                installedOf, {kOtherDid}, reservedFor({}, {}, impostor));
        QCOMPARE(judged.last().name, QStringLiteral("eth_wallet_ui_backend"));
        QVERIFY(judged.last().reserved);
        QCOMPARE(judged.last().status, Status::Invalid);
    }

    // The other order: a package already installed under the name an app's
    // backend runs as. The app would not get its backend, and that package
    // would pass for it, so the app doesn't install, whoever signed it.
    void anAppWhoseBackendNameIsTakenDoesNotInstall()
    {
        const auto onDisk = [](const QVariantMap&) { return false; };
        const auto versionsOf = [](const QVariantMap& row) {
            return QVariantList{release(row.value(QStringLiteral("name")).toString(),
                                        row.value(QStringLiteral("version")).toString(),
                                        row.value(QStringLiteral("rootHash")).toString(), logosDid())};
        };
        const auto installedOf = [](const QVariantMap& row) {
            return row.value(QStringLiteral("name")).toString() == QLatin1String("chat_ui_backend")
                ? QVariantMap{{QStringLiteral("installType"), QStringLiteral("user")},
                              {QStringLiteral("type"), QStringLiteral("core")}}
                : QVariantMap{};
        };
        QVariantMap app = planRow(QStringLiteral("chat_ui"), QStringLiteral("1.0.0"), QStringLiteral("a"));
        app[QStringLiteral("type")] = QStringLiteral("ui_qml");
        const QList<Package> taken = judgePlan(QStringLiteral("chat_ui"), {app}, onDisk, versionsOf,
                                               installedOf, {logosDid()});
        QCOMPARE(taken.size(), 1);
        QVERIFY(taken[0].backendTaken);
        QCOMPARE(taken[0].status, Status::Invalid);

        // A core module of that name is a package like any other.
        QVariantMap core = planRow(QStringLiteral("chat_ui"), QStringLiteral("1.0.0"), QStringLiteral("a"));
        core[QStringLiteral("type")] = QStringLiteral("core");
        const QList<Package> fine = judgePlan(QStringLiteral("chat_ui"), {core}, onDisk, versionsOf,
                                              installedOf, {logosDid()});
        QVERIFY(!fine[0].backendTaken);
        QCOMPARE(fine[0].status, Status::Trusted);
    }

    // A local file is judged as a catalogue row is: an app whose backend's
    // name an installed package has doesn't install from a file either.
    void aLocalAppWhoseBackendNameIsTakenDoesNotInstall()
    {
        const QVariantMap backendName{{QStringLiteral("installType"), QStringLiteral("user")},
                                      {QStringLiteral("type"), QStringLiteral("core")}};
        const Package file = fromVerifiedFile(QStringLiteral("chat_ui"), QStringLiteral("1.0.0"),
                                              verified(true, true, true, logosDid()), {logosDid()});
        QCOMPARE(file.status, Status::Trusted);
        const Package app = asInstall(file, QStringLiteral("ui_qml"), {}, backendName, {});
        QCOMPARE(app.status, Status::Invalid);
        QVERIFY(app.backendTaken);
        QCOMPARE(asInstall(file, QStringLiteral("core"), {}, backendName, {}).status, Status::Trusted);
        QCOMPARE(asInstall(file, QStringLiteral("ui_qml"), {}, {}, {}).status, Status::Trusted);
        // And it is judged as replacing what it would replace.
        const Package shell = asInstall(fromVerifiedFile(QStringLiteral("main_ui"), QStringLiteral("1.0.0"),
                                                         verified(true, true, true, logosDid()), {logosDid()}),
                                        QStringLiteral("ui_qml"), {}, {}, reservedNames({}, {}));
        QCOMPARE(shell.status, Status::Invalid);
        QVERIFY(shell.builtIn);
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

    // Basecamp's own Logos Storage is Logos's: another publisher's copy takes
    // its place only asked, signed by a key the person trusts too. It used to
    // install unasked: the bundled copy has no signer on record.
    void replacingTheBundledCopyAsksUnlessLogosSignedIt()
    {
        const QVariantMap bundledCopy{{QStringLiteral("installType"), QStringLiteral("embedded")}};
        const QVariantList keys{QVariantMap{{QStringLiteral("did"), kOtherDid},
                                            {QStringLiteral("displayName"), QStringLiteral("Other")}}};
        const Package trusted = asReplacement(
            {QStringLiteral("storage_module"), QStringLiteral("3.0.2"), Status::Trusted, kOtherDid,
             QStringLiteral("Other")},
            bundledCopy, {});
        QCOMPARE(trusted.replacesSignedBy, logosDid());
        const QVariantMap s = summary({trusted}, keys, QStringLiteral("storage_module"));
        QCOMPARE(s.value(QStringLiteral("needsPrompt")).toBool(), true);
        QCOMPARE(s.value(QStringLiteral("canTrust")).toBool(), false);
        QCOMPARE(s.value(QStringLiteral("byName")).toMap().value(QStringLiteral("storage_module")).toMap()
                     .value(QStringLiteral("replaces")).toString(), logosName());
        QCOMPARE(asReplacement({QStringLiteral("storage_module"), QStringLiteral("3.0.2"), Status::Unsigned, {}},
                               bundledCopy, {}).replacesSignedBy, logosDid());

        // Logos's, with any of its keys: nothing changes hands.
        QVERIFY(asReplacement({QStringLiteral("storage_module"), {}, Status::Trusted, logosDid()}, bundledCopy, {})
                    .replacesSignedBy.isEmpty());
        const LogosKeys rotated{{kThirdDid, logosDid()}, {}};
        QVERIFY(asReplacement({QStringLiteral("storage_module"), {}, Status::Trusted, logosDid()}, bundledCopy, {},
                              rotated).replacesSignedBy.isEmpty());
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

    // An unsigned release's manifest is outside its rootHash: the plan pins
    // the file by the catalogue's digest, so what it declares can't change
    // once approved. A signed one is pinned by its signature.
    void anUnsignedReleaseIsPinnedByItsDigest()
    {
        const QVariantList resolved{planRow(QStringLiteral("loose"), QStringLiteral("1.0.0"), QStringLiteral("l")),
                                    planRow(QStringLiteral("app"), QStringLiteral("2.0.0"), QStringLiteral("a"))};
        const auto versionsOf = [](const QVariantMap& row) {
            const QString name = row.value(QStringLiteral("name")).toString();
            QVariantMap entry = release(name, row.value(QStringLiteral("version")).toString(),
                                        row.value(QStringLiteral("rootHash")).toString(),
                                        name == QLatin1String("app") ? logosDid() : QString());
            entry[QStringLiteral("sha256")] = QStringLiteral("digest-of-") + name;
            return QVariantList{entry};
        };
        const QList<Package> judged = judgePlan(
            QStringLiteral("app"), resolved, [](const QVariantMap&) { return false; }, versionsOf,
            [](const QVariantMap&) { return QVariantMap{}; }, {logosDid()});
        const QVariantList plan = approvedPlan(QStringLiteral("app"), resolved, judged);
        QCOMPARE(plan.at(0).toMap().value(QStringLiteral("sha256")).toString(), QStringLiteral("digest-of-loose"));
        QVERIFY(!plan.at(1).toMap().contains(QStringLiteral("sha256")));
    }






};

QTEST_GUILESS_MAIN(InstallTrustTest)
#include "install_trust_test.moc"
