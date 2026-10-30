// srcdeps: utils/InstalledIntegrity.cpp utils/InstalledPlacement.cpp
// libdeps: lgx
//
// Unit tests for the launch-time integrity check itself
// (app/utils/InstalledIntegrity.{h,cpp}): problemWith() against real packages,
// built and installed with liblgx the way lgpm lays one out -- the variant's
// files, plus manifest.json, manifest.sig when signed, and a `variant` file.

#include "../app/utils/InstalledIntegrity.h"

#include <lgx.h>

#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace logos::integrity;

namespace {

constexpr const char* kVariant = "linux-amd64";

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class InstalledIntegrityTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tmp;
    int serial = 0;

    // Builds a one-variant package from scratch and installs it into a fresh
    // modules dir. Returns the package directory.
    QString install(bool sign = false)
    {
        const QString work = tmp.filePath(QStringLiteral("w%1").arg(++serial));
        const QString content = work + QStringLiteral("/content");
        writeFile(content + QStringLiteral("/app_plugin.so"), "plugin");
        writeFile(content + QStringLiteral("/qml/Main.qml"), "import QtQuick\nItem {}\n");

        const QByteArray lgxPath = QFile::encodeName(work + QStringLiteral("/app.lgx"));
        if (!lgx_create(lgxPath.constData(), "app").success) return {};
        lgx_package_t pkg = lgx_load(lgxPath.constData());
        if (!pkg) return {};
        const bool added =
            lgx_add_variant(pkg, kVariant, QFile::encodeName(content).constData(), "app_plugin.so").success
            && lgx_save(pkg, lgxPath.constData()).success;
        lgx_free_package(pkg);
        if (!added) return {};

        if (sign) {
            const QByteArray keys = QFile::encodeName(work + QStringLiteral("/keys"));
            QDir().mkpath(QString::fromUtf8(keys));
            if (!lgx_keygen("publisher", keys.constData()).success) return {};
            if (!lgx_sign(lgxPath.constData(), (keys + "/publisher.jwk").constData(),
                          nullptr, nullptr).success)
                return {};
        }

        pkg = lgx_load(lgxPath.constData());
        if (!pkg) return {};
        const QString staging = work + QStringLiteral("/staging");
        const bool extracted =
            lgx_extract(pkg, kVariant, QFile::encodeName(staging).constData()).success;
        const QString dir = work + QStringLiteral("/modules/app");
        QDir().mkpath(work + QStringLiteral("/modules"));
        if (extracted && QDir().rename(staging + QLatin1Char('/') + QLatin1String(kVariant), dir)) {
            writeFile(dir + QStringLiteral("/manifest.json"), lgx_get_manifest_json(pkg));
            writeFile(dir + QStringLiteral("/variant"), kVariant);
            if (const char* sig = lgx_get_manifest_sig_json(pkg))
                writeFile(dir + QStringLiteral("/manifest.sig"), sig);
        }
        lgx_free_package(pkg);
        return QFileInfo::exists(dir + QStringLiteral("/manifest.json")) ? dir : QString();
    }

private slots:
    void initTestCase() { QVERIFY(tmp.isValid()); }

    void anUntouchedInstallIsClean()
    {
        const QString dir = install();
        QVERIFY(!dir.isEmpty());
        QCOMPARE(problemWith(dir), QString());
    }

    void aModifiedFileIsRefused()
    {
        const QString dir = install();
        writeFile(dir + QStringLiteral("/app_plugin.so"), "edited");
        QVERIFY2(problemWith(dir).contains(QStringLiteral("modified or damaged")), qPrintable(problemWith(dir)));
    }

    void anAddedFileIsRefused()
    {
        const QString dir = install();
        writeFile(dir + QStringLiteral("/qml/Injected.qml"), "import QtQuick\nItem {}\n");
        QVERIFY(problemWith(dir).contains(QStringLiteral("modified or damaged")));
    }

    // What lgpm writes beside the payload is not content.
    void theInstallSourceSidecarIsClean()
    {
        const QString dir = install();
        writeFile(dir + QStringLiteral("/.lgpm-source"), "https://example.org/repo\n");
        QCOMPARE(problemWith(dir), QString());
    }

    void aManifestWithoutHashesIsRefused()
    {
        const QString dir = install();
        QJsonObject m = QJsonDocument::fromJson(readFile(dir + QStringLiteral("/manifest.json"))).object();
        m.remove(QStringLiteral("hashes"));
        writeFile(dir + QStringLiteral("/manifest.json"), QJsonDocument(m).toJson());
        QVERIFY(problemWith(dir).contains(QStringLiteral("no content hashes")));
    }

    void aMissingManifestOrVariantIsRefused()
    {
        const QString a = install();
        QFile::remove(a + QStringLiteral("/manifest.json"));
        QVERIFY(problemWith(a).contains(QStringLiteral("manifest.json is missing")));

        const QString b = install();
        QFile::remove(b + QStringLiteral("/variant"));
        QVERIFY(problemWith(b).contains(QStringLiteral("variant file is missing")));

        QVERIFY(problemWith(tmp.filePath(QStringLiteral("nowhere"))).contains(QStringLiteral("missing")));
    }

    void anUntouchedSignedInstallIsClean()
    {
        const QString dir = install(/*sign=*/true);
        QVERIFY(!dir.isEmpty());
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/manifest.sig")));
        QCOMPARE(problemWith(dir), QString());
    }

    // The case the signature check exists for: the files still match the
    // manifest, but the manifest is no longer the one that was signed.
    void aRewrittenSignedManifestIsRefused()
    {
        const QString dir = install(/*sign=*/true);
        QJsonObject m = QJsonDocument::fromJson(readFile(dir + QStringLiteral("/manifest.json"))).object();
        m.insert(QStringLiteral("description"), QStringLiteral("rewritten"));
        writeFile(dir + QStringLiteral("/manifest.json"), QJsonDocument(m).toJson(QJsonDocument::Compact));
        QVERIFY2(problemWith(dir).contains(QStringLiteral("signature no longer matches")),
                 qPrintable(problemWith(dir)));
    }

    void anUnreadableSignatureIsRefused()
    {
        const QString dir = install(/*sign=*/true);
        writeFile(dir + QStringLiteral("/manifest.sig"), "not a signature");
        QVERIFY(problemWith(dir).contains(QStringLiteral("signature file is unreadable")));
    }

    // Documented limit, not a guarantee: deleting manifest.sig makes a signed
    // install read as unsigned. Doing that AND rewriting the hashes needs write
    // access to the user's files, which the check does not defend against.
    void aDeletedSignatureReadsAsUnsigned()
    {
        const QString dir = install(/*sign=*/true);
        QFile::remove(dir + QStringLiteral("/manifest.sig"));
        QCOMPARE(problemWith(dir), QString());
    }

    // What kind of failure it is decides what the user is told to do.
    void eachFailureSaysWhatKindItIs()
    {
        using Kind = Problem::Kind;
        const QString modified = install();
        writeFile(modified + QStringLiteral("/app_plugin.so"), "edited");
        QCOMPARE(examine(modified).kind, Kind::Changed);

        const QString rewritten = install(/*sign=*/true);
        QJsonObject m = QJsonDocument::fromJson(readFile(rewritten + QStringLiteral("/manifest.json"))).object();
        m.insert(QStringLiteral("description"), QStringLiteral("rewritten"));
        writeFile(rewritten + QStringLiteral("/manifest.json"), QJsonDocument(m).toJson(QJsonDocument::Compact));
        QCOMPARE(examine(rewritten).kind, Kind::Changed);

        const QString noHashes = install();
        QJsonObject n = QJsonDocument::fromJson(readFile(noHashes + QStringLiteral("/manifest.json"))).object();
        n.remove(QStringLiteral("hashes"));
        writeFile(noHashes + QStringLiteral("/manifest.json"), QJsonDocument(n).toJson());
        QCOMPARE(examine(noHashes).kind, Kind::Unverifiable);

        const QString noManifest = install();
        QFile::remove(noManifest + QStringLiteral("/manifest.json"));
        QCOMPARE(examine(noManifest).kind, Kind::Unverifiable);

        const QString garbageSig = install(/*sign=*/true);
        writeFile(garbageSig + QStringLiteral("/manifest.sig"), "not a signature");
        QCOMPARE(examine(garbageSig).kind, Kind::Unverifiable);

        QVERIFY(examine(install()).isEmpty());
    }

    // At startup only the copy of a module that will run is checked: the
    // package manager runs a later copy unless an earlier one is newer.
    void theCopyThatRunsIsTheOneThePackageManagerPicks()
    {
        const QString base = tmp.filePath(QStringLiteral("copies%1").arg(++serial));
        const QString bundled = base + QStringLiteral("/bundled");
        const QString user = base + QStringLiteral("/user");
        const QString profile = base + QStringLiteral("/profile");
        auto put = [](const QString& dir, const QByteArray& version, const QByteArray& name = "mod") {
            writeFile(dir + QStringLiteral("/mod/manifest.json"),
                      "{\"name\":\"" + name + "\",\"type\":\"core\",\"version\":\"" + version + "\"}");
        };
        const QString inUser = QDir(user).filePath(QStringLiteral("mod"));
        const QString inBundled = QDir(bundled).filePath(QStringLiteral("mod"));

        put(user, "1.0.0");
        QCOMPARE(copyThatRuns(QStringLiteral("mod"), {bundled, user}), inUser);

        put(bundled, "1.0.0");
        QCOMPARE(copyThatRuns(QStringLiteral("mod"), {bundled, user}), inUser);   // a tie goes to the later

        put(user, "1.1.0");
        QCOMPARE(copyThatRuns(QStringLiteral("mod"), {bundled, user}), inUser);

        put(bundled, "1.2.0");
        QCOMPARE(copyThatRuns(QStringLiteral("mod"), {bundled, user}), inBundled);  // never shadowed by older

        put(bundled, "1.0.0");
        put(user, "not-a-version");
        QCOMPARE(copyThatRuns(QStringLiteral("mod"), {bundled, user}), inBundled);  // junk sorts lowest

        put(user, "1.1.0", "other");
        QCOMPARE(copyThatRuns(QStringLiteral("mod"), {bundled, user}), inBundled);  // not a copy of mod

        put(user, "1.1.0");
        put(profile, "1.1.0");
        QCOMPARE(copyThatRuns(QStringLiteral("mod"), {bundled, user, profile}),
                 QDir(profile).filePath(QStringLiteral("mod")));

        QFile::remove(inUser + QStringLiteral("/manifest.json"));
        QCOMPARE(copyThatRuns(QStringLiteral("mod"), {bundled, user}), inBundled);
        QCOMPARE(copyThatRuns(QStringLiteral("absent"), {bundled, user}), QString());
    }
};

QTEST_GUILESS_MAIN(InstalledIntegrityTest)
#include "installed_integrity_test.moc"
