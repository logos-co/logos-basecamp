// Unit tests for src/UpdateInfo.h — the pure logic behind the host-app update
// check (issue #319).
//
// This file deliberately declares no extra production sources (see the
// first-line directive tests/CMakeLists.txt greps for). UpdateInfo.h is
// header-only by design, precisely so this test needs neither Qt6::Network nor
// app/utils — tests/CMakeLists.txt links and includes neither. If a future
// change here forces one to be added, that is a signal the logic has drifted
// back into the I/O shell.
//
//   nix build .#unit-tests -L

#include "UpdateInfo.h"

#include <QtTest/QtTest>

#include <logos/semver.hpp>

using namespace UpdateInfo;

namespace {

// ── Fixture: the real v0.2.3 release's assets[] ──────────────────────────────
//
// Verbatim from https://github.com/logos-co/logos-basecamp/releases/tag/0.2.3.
// Three assets, two architectures, two package formats — and crucially NO
// x86_64 .dmg. The names carry "-aarch64" while QSysInfo::currentCpuArchitecture()
// reports "arm64" on Apple Silicon; that mismatch is the whole reason
// archToken() exists.
Asset makeAsset(const QString& name, qint64 size,
                const QString& state = QStringLiteral("uploaded"))
{
    Asset a;
    a.name  = name;
    a.url   = QStringLiteral(
                  "https://github.com/logos-co/logos-basecamp/releases/download/0.2.3/%1")
                  .arg(name);
    a.size  = size;
    a.state = state;
    return a;
}

QList<Asset> release023Assets()
{
    return {
        makeAsset(QStringLiteral("LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.AppImage"),
                  271104520),
        makeAsset(QStringLiteral("LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg"),
                  98701839),
        makeAsset(QStringLiteral("LogosBasecamp-Desktop-v0.2.3-aa2377-x86_64.AppImage"),
                  278084088),
    };
}

const char* kAarch64AppImage = "LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.AppImage";
const char* kAarch64Dmg      = "LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg";
const char* kX86AppImage     = "LogosBasecamp-Desktop-v0.2.3-aa2377-x86_64.AppImage";

// moc constraint — do NOT rewrite these JSON fixtures as R"json(...)json" raw
// string literals, however much more readable that would be.
//
// moc does not understand raw string literals. It sees the "//" in every
// "https://" URL below as the start of a line comment, discards the rest of the
// line including the closing )json" delimiter, and then reports a bewildering
// `missing ')' in macro usage` on some later line — or silently swallows the
// rest of the translation unit, yielding an empty .moc and a link error about
// UpdateInfoTest::staticMetaObject with nothing pointing at the JSON.
//
// It reproduces under nix/unit-tests.nix's moc invocation but NOT under a bare
// `moc file.cpp`, so it will not show up in local spot-checks — only in CI.
// Ordinary escaped literals lex correctly; keep them.

// A trimmed but structurally faithful /releases/latest payload.
QByteArray release023Json()
{
    return QByteArrayLiteral(
        "{"
        "  \"tag_name\": \"0.2.3\","
        "  \"name\": \"0.2.3\","
        "  \"html_url\": \"https://github.com/logos-co/logos-basecamp/releases/tag/0.2.3\","
        "  \"assets\": ["
        "    {"
        "      \"name\": \"LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.AppImage\","
        "      \"browser_download_url\": \"https://github.com/logos-co/logos-basecamp/releases/download/0.2.3/LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.AppImage\","
        "      \"size\": 271104520,"
        "      \"state\": \"uploaded\","
        "      \"content_type\": \"application/octet-stream\""
        "    },"
        "    {"
        "      \"name\": \"LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg\","
        "      \"browser_download_url\": \"https://github.com/logos-co/logos-basecamp/releases/download/0.2.3/LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg\","
        "      \"size\": 98701839,"
        "      \"state\": \"uploaded\","
        "      \"content_type\": \"application/octet-stream\""
        "    },"
        "    {"
        "      \"name\": \"LogosBasecamp-Desktop-v0.2.3-aa2377-x86_64.AppImage\","
        "      \"browser_download_url\": \"https://github.com/logos-co/logos-basecamp/releases/download/0.2.3/LogosBasecamp-Desktop-v0.2.3-aa2377-x86_64.AppImage\","
        "      \"size\": 278084088,"
        "      \"state\": \"uploaded\","
        "      \"content_type\": \"application/octet-stream\""
        "    }"
        "  ]"
        "}");
}

} // namespace

class UpdateInfoTest : public QObject
{
    Q_OBJECT

private slots:
    // ── asset selection ──────────────────────────────────────────────────
    void selectAssetName_realRelease_data();
    void selectAssetName_realRelease();
    void selectAssetName_skipsNonUploadedAssets();
    void selectAssetName_emptyStateTreatedAsUploaded();
    void selectAssetName_ignoresForeignFilenames();
    void selectAssetName_emptyAssetList();
    void archToken_mapsHostNamesToAssetNames_data();
    void archToken_mapsHostNamesToAssetNames();

    // ── version handling ─────────────────────────────────────────────────
    void normalizeVersion_peelsLeadingV();
    void isComparableVersion_data();
    void isComparableVersion();
    void rawComparatorWouldFalselyOfferUpdateToEveryDevBuild();
    void isNewer_data();
    void isNewer();

    // ── release payload ──────────────────────────────────────────────────
    void parseRelease_roundTripsRealPayload();
    void parseRelease_rejectsBadInput_data();
    void parseRelease_rejectsBadInput();
    void parseRelease_skipsAssetsWithoutName();

    // ── transport safety ─────────────────────────────────────────────────
    void isTrustedReleaseUrl_data();
    void isTrustedReleaseUrl();
    void isAllowedDownloadHost_data();
    void isAllowedDownloadHost();

    // ── presentation ─────────────────────────────────────────────────────
    void formatBytes_data();
    void formatBytes();
    void progressText_showsBothSizesAndPercent();
    void progressText_unknownTotalFallsBackToReceivedOnly();
    void installHint_data();
    void installHint();

    // ── download decisions (extracted from UpdateChecker's I/O shell) ────
    void isAllowedDownloadUrl_requiresHttpsAndAllowedHost_data();
    void isAllowedDownloadUrl_requiresHttpsAndAllowedHost();
    void isAcceptableHttpStatus_data();
    void isAcceptableHttpStatus();
    void isDeclaredSizeMismatch_data();
    void isDeclaredSizeMismatch();
    void isCompleteDownload_data();
    void isCompleteDownload();
    void hasEnoughSpaceFor_data();
    void hasEnoughSpaceFor();
    void dedupedFileName_data();
    void dedupedFileName();

    // ── security regressions ────────────────────────────────────────────
    void isSafeAssetFileName_data();
    void isSafeAssetFileName();
    void selectAssetName_rejectsPathTraversalDespiteMatchingPrefixAndSuffix();
    void isAllowedDownloadHost_rejectsArbitraryS3Buckets();
    void shouldSkipCheck_data();
    void shouldSkipCheck();
    void downloadFraction_data();
    void downloadFraction();
};

// ── asset selection ─────────────────────────────────────────────────────────

void UpdateInfoTest::selectAssetName_realRelease_data()
{
    QTest::addColumn<QString>("os");
    QTest::addColumn<QString>("cpu");
    QTest::addColumn<QString>("expected");

    // THE trap: QSysInfo::currentCpuArchitecture() says "arm64", the asset
    // says "aarch64". Without archToken() this returns empty on every Mac and
    // every ARM Linux box — i.e. the feature ships silently doing nothing.
    QTest::newRow("apple-silicon-mac -> dmg")
        << "macos" << "arm64" << QString::fromLatin1(kAarch64Dmg);
    QTest::newRow("arm64 linux -> aarch64 AppImage")
        << "linux" << "arm64" << QString::fromLatin1(kAarch64AppImage);
    // Defensive: some hosts/toolchains report the kernel spelling directly.
    QTest::newRow("aarch64 linux -> aarch64 AppImage")
        << "linux" << "aarch64" << QString::fromLatin1(kAarch64AppImage);
    QTest::newRow("x86_64 linux -> x86_64 AppImage")
        << "linux" << "x86_64" << QString::fromLatin1(kX86AppImage);
    QTest::newRow("amd64 alias -> x86_64 AppImage")
        << "linux" << "amd64" << QString::fromLatin1(kX86AppImage);

    // Intel Mac: 0.2.3 genuinely publishes no x86_64 .dmg. Empty is the
    // correct answer and a UI state (link out to the release page), not a bug.
    QTest::newRow("intel-mac has no dmg published")
        << "macos" << "x86_64" << QString();

    // Windows ships nothing at all, on any arch.
    QTest::newRow("windows arm64")  << "windows" << "arm64"  << QString();
    QTest::newRow("windows x86_64") << "windows" << "x86_64" << QString();

    // Unknown arch must not fall through to "pick something".
    QTest::newRow("riscv64 linux")  << "linux"   << "riscv64" << QString();

    // Empty inputs are the "we don't know the host" case.
    QTest::newRow("empty os")   << QString() << "arm64"    << QString();
    QTest::newRow("empty arch") << "linux"   << QString()  << QString();
}

void UpdateInfoTest::selectAssetName_realRelease()
{
    QFETCH(QString, os);
    QFETCH(QString, cpu);
    QFETCH(QString, expected);

    const QString actual = selectAssetName(release023Assets(), os, cpu);
    QCOMPARE(actual, expected);
}

void UpdateInfoTest::selectAssetName_skipsNonUploadedAssets()
{
    // GitHub reports state "starting" while it is still ingesting the upload.
    // Downloading one yields a truncated file that fails to mount/run, so the
    // only safe answer is "no asset yet".
    QList<Asset> assets{
        makeAsset(QStringLiteral("LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg"),
                  98701839, QStringLiteral("starting")),
    };
    QCOMPARE(selectAssetName(assets, QStringLiteral("macos"),
                             QStringLiteral("arm64")),
             QString());

    // With an uploaded sibling present, selection resumes normally.
    assets.append(makeAsset(
        QStringLiteral("LogosBasecamp-Desktop-v0.2.3-bb1188-aarch64.dmg"),
        98701839));
    QCOMPARE(selectAssetName(assets, QStringLiteral("macos"),
                             QStringLiteral("arm64")),
             QStringLiteral("LogosBasecamp-Desktop-v0.2.3-bb1188-aarch64.dmg"));
}

void UpdateInfoTest::selectAssetName_emptyStateTreatedAsUploaded()
{
    // Not every producer of this struct fills `state` (e.g. a hand-built
    // fixture, or a payload variant without the field). Empty must not be
    // read as "not ready" — that would disable updates wholesale.
    QList<Asset> assets{
        makeAsset(QStringLiteral("LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg"),
                  98701839, QString()),
    };
    QCOMPARE(selectAssetName(assets, QStringLiteral("macos"),
                             QStringLiteral("arm64")),
             QStringLiteral("LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg"));
}

void UpdateInfoTest::selectAssetName_ignoresForeignFilenames()
{
    // Releases carry extra attachments (checksums, source archives, sidecar
    // builds). Only LogosBasecamp-Desktop-v* is ours; matching purely on the
    // "-<arch>.<ext>" suffix would happily hand back someone else's binary.
    const QList<Asset> assets{
        makeAsset(QStringLiteral("SomeOtherApp-v0.2.3-aa2377-aarch64.dmg"), 123),
        makeAsset(QStringLiteral("LogosBasecamp-Server-v0.2.3-aa2377-aarch64.dmg"), 123),
        makeAsset(QStringLiteral("aarch64.dmg"), 123),
        makeAsset(QStringLiteral("LogosBasecamp-Desktop-0.2.3-aa2377-aarch64.dmg"), 123),
    };
    QVERIFY2(selectAssetName(assets, QStringLiteral("macos"),
                             QStringLiteral("arm64")).isEmpty(),
             "only assets named LogosBasecamp-Desktop-v* may be selected");
}

void UpdateInfoTest::selectAssetName_emptyAssetList()
{
    QCOMPARE(selectAssetName({}, QStringLiteral("macos"), QStringLiteral("arm64")),
             QString());
}

void UpdateInfoTest::archToken_mapsHostNamesToAssetNames_data()
{
    QTest::addColumn<QString>("cpu");
    QTest::addColumn<QString>("expected");

    QTest::newRow("arm64")   << "arm64"   << "aarch64";
    QTest::newRow("aarch64") << "aarch64" << "aarch64";
    QTest::newRow("ARM64")   << "ARM64"   << "aarch64";   // case-insensitive
    QTest::newRow("x86_64")  << "x86_64"  << "x86_64";
    QTest::newRow("amd64")   << "amd64"   << "x86_64";
    QTest::newRow("i386")    << "i386"    << QString();
    QTest::newRow("riscv64") << "riscv64" << QString();
    QTest::newRow("empty")   << QString() << QString();
}

void UpdateInfoTest::archToken_mapsHostNamesToAssetNames()
{
    QFETCH(QString, cpu);
    QFETCH(QString, expected);
    QCOMPARE(archToken(cpu), expected);
}

// ── version handling ────────────────────────────────────────────────────────

void UpdateInfoTest::normalizeVersion_peelsLeadingV()
{
    // Tags are bare ("0.2.3"); asset filenames embed "v0.2.3". Both reach the
    // comparator, and strict semver rejects the prefix.
    QCOMPARE(normalizeVersion(QStringLiteral("v0.2.3")), QStringLiteral("0.2.3"));
    QCOMPARE(normalizeVersion(QStringLiteral("V0.2.3")), QStringLiteral("0.2.3"));
    QCOMPARE(normalizeVersion(QStringLiteral("0.2.3")),  QStringLiteral("0.2.3"));
    QCOMPARE(normalizeVersion(QStringLiteral("  0.2.3  ")), QStringLiteral("0.2.3"));
    QCOMPARE(normalizeVersion(QStringLiteral(" v0.2.3 ")), QStringLiteral("0.2.3"));
    QCOMPARE(normalizeVersion(QString()), QString());
}

void UpdateInfoTest::isComparableVersion_data()
{
    QTest::addColumn<QString>("current");
    QTest::addColumn<bool>("expected");

    QTest::newRow("release tag")    << "0.2.3"    << true;
    QTest::newRow("rc tag")         << "0.2.3-RC1" << true;
    QTest::newRow("v-prefixed")     << "v0.2.3"   << true;
    QTest::newRow("build metadata") << "0.2.3+aa2377" << true;

    // The three shapes a non-release build actually takes:
    //   master CI  → "pre-release-<sha7>"
    //   dirty local build → ""
    //   ad-hoc → anything unparseable
    QTest::newRow("master CI build") << "pre-release-aa23776" << false;
    QTest::newRow("dirty local")     << ""                    << false;
    QTest::newRow("dev")             << "dev"                 << false;
    QTest::newRow("partial 0.2")     << "0.2"                 << false;
}

void UpdateInfoTest::isComparableVersion()
{
    QFETCH(QString, current);
    QFETCH(bool, expected);
    QCOMPARE(UpdateInfo::isComparableVersion(current), expected);
}

void UpdateInfoTest::rawComparatorWouldFalselyOfferUpdateToEveryDevBuild()
{
    // THIS is why isComparableVersion() exists, pinned so nobody "simplifies"
    // isNewer() down to a bare compare().
    //
    // logos::semver::compare sorts an UNPARSEABLE string BELOW every parseable
    // one (see semver.hpp: `if (vb) return -1;`). So the raw comparator claims
    // "pre-release-aa23776" < "0.2.3" — i.e. every master CI build and every
    // dirty local build would be told, forever, that it is out of date.
    QVERIFY2(logos::semver::compare("pre-release-aa23776", "0.2.3") < 0,
             "precondition: the raw comparator ranks a dev build below 0.2.3");
    QVERIFY2(logos::semver::compare("", "0.2.3") < 0,
             "precondition: the raw comparator ranks an empty version below 0.2.3");

    // The gate inside isNewer() suppresses both.
    QVERIFY2(!UpdateInfo::isNewer(QStringLiteral("pre-release-aa23776"),
                                  QStringLiteral("0.2.3")),
             "a dev build must never be told an update is available");
    QVERIFY2(!UpdateInfo::isNewer(QString(), QStringLiteral("0.2.3")),
             "a dirty local build must never be told an update is available");
}

void UpdateInfoTest::isNewer_data()
{
    QTest::addColumn<QString>("current");
    QTest::addColumn<QString>("latest");
    QTest::addColumn<bool>("expected");

    QTest::newRow("older -> newer")   << "0.2.1" << "0.2.3" << true;
    QTest::newRow("same version")     << "0.2.3" << "0.2.3" << false;

    // Never offer a downgrade: /releases/latest is ordered by creation date,
    // not semver precedence, so a hotfix cut from an old branch can be
    // "latest" while being older than what the user is running.
    QTest::newRow("downgrade refused") << "0.2.4" << "0.2.3" << false;

    // Prerelease precedence: 0.2.3-RC1 < 0.2.3.
    QTest::newRow("rc -> release")     << "0.2.3-RC1" << "0.2.3"     << true;
    QTest::newRow("release -> next rc")<< "0.2.3"     << "0.2.4-RC1" << true;
    QTest::newRow("release -> same-version rc is a downgrade")
        << "0.2.3" << "0.2.3-RC1" << false;

    // The `v` prefix must be tolerated on either side or both.
    QTest::newRow("v on latest")  << "0.2.1"  << "v0.2.3" << true;
    QTest::newRow("v on current") << "v0.2.1" << "0.2.3"  << true;
    QTest::newRow("v on both")    << "v0.2.1" << "v0.2.3" << true;

    // Upstream data is parsed leniently: a partial tag is unambiguous.
    QTest::newRow("lenient latest 1.0") << "0.2.3" << "1.0" << true;

    // The running build is held to the strict standard — see the gate test.
    QTest::newRow("dev current")   << "pre-release-aa23776" << "0.2.3" << false;
    QTest::newRow("empty current") << ""                    << "0.2.3" << false;
    // And junk upstream data must not be read as "newer" either.
    QTest::newRow("junk latest")   << "0.2.3" << "banana" << false;
    QTest::newRow("empty latest")  << "0.2.3" << ""       << false;
}

void UpdateInfoTest::isNewer()
{
    QFETCH(QString, current);
    QFETCH(QString, latest);
    QFETCH(bool, expected);
    QCOMPARE(UpdateInfo::isNewer(current, latest), expected);
}

// ── release payload ─────────────────────────────────────────────────────────

void UpdateInfoTest::parseRelease_roundTripsRealPayload()
{
    const Release r = parseRelease(release023Json());

    QVERIFY2(r.ok, "a well-formed release payload must parse");
    QCOMPARE(r.tag, QStringLiteral("0.2.3"));
    QCOMPARE(r.htmlUrl,
             QStringLiteral("https://github.com/logos-co/logos-basecamp/releases/tag/0.2.3"));
    QCOMPARE(r.assets.size(), 3);

    QCOMPARE(r.assets.at(0).name,  QString::fromLatin1(kAarch64AppImage));
    QCOMPARE(r.assets.at(0).size,  Q_INT64_C(271104520));
    QCOMPARE(r.assets.at(0).state, QStringLiteral("uploaded"));
    QCOMPARE(r.assets.at(0).url,
             QStringLiteral("https://github.com/logos-co/logos-basecamp/releases/"
                            "download/0.2.3/") + QString::fromLatin1(kAarch64AppImage));

    QCOMPARE(r.assets.at(1).name, QString::fromLatin1(kAarch64Dmg));
    QCOMPARE(r.assets.at(1).size, Q_INT64_C(98701839));

    QCOMPARE(r.assets.at(2).name, QString::fromLatin1(kX86AppImage));
    QCOMPARE(r.assets.at(2).size, Q_INT64_C(278084088));

    // The two halves compose: parse the payload, then pick for this host.
    QCOMPARE(selectAssetName(r.assets, QStringLiteral("macos"),
                             QStringLiteral("arm64")),
             QString::fromLatin1(kAarch64Dmg));

    // 271104520 is comfortably past the 2^31 boundary — assert it survived the
    // double round-trip in parseRelease intact rather than truncating.
    QVERIFY2(r.assets.at(2).size > Q_INT64_C(2147483647) / 8,
             "sanity: sizes are large; they must not be parsed into an int");
}

void UpdateInfoTest::parseRelease_rejectsBadInput_data()
{
    QTest::addColumn<QByteArray>("json");

    QTest::newRow("empty")           << QByteArray();
    QTest::newRow("whitespace")      << QByteArrayLiteral("   ");
    QTest::newRow("malformed json")  << QByteArrayLiteral("{ \"tag_name\": ");
    QTest::newRow("not json at all") << QByteArrayLiteral("<html>404</html>");
    // GitHub returns an ARRAY from /releases (plural). Hitting the wrong
    // endpoint must fail closed, not half-parse.
    QTest::newRow("array not object")
        << QByteArrayLiteral("[{\"tag_name\":\"0.2.3\"}]");
    QTest::newRow("empty array")  << QByteArrayLiteral("[]");
    QTest::newRow("json null")    << QByteArrayLiteral("null");
    // No tag_name → nothing to compare against, so there is no update to offer.
    QTest::newRow("no tag_name")
        << QByteArrayLiteral("{\"html_url\":\"https://example.com\",\"assets\":[]}");
    QTest::newRow("empty tag_name")
        << QByteArrayLiteral("{\"tag_name\":\"\",\"assets\":[]}");
    // html_url is required for the same reason tag_name is: UpdateChecker gates
    // trust on isTrustedReleaseUrl(), so accepting a payload that merely OMITS
    // the field would let it skip that check entirely — failing open.
    QTest::newRow("no html_url")
        << QByteArrayLiteral("{\"tag_name\":\"0.2.3\",\"assets\":[]}");
    QTest::newRow("empty html_url")
        << QByteArrayLiteral("{\"tag_name\":\"0.2.3\",\"html_url\":\"\",\"assets\":[]}");
}

void UpdateInfoTest::parseRelease_rejectsBadInput()
{
    QFETCH(QByteArray, json);
    const Release r = parseRelease(json);
    QVERIFY2(!r.ok, "malformed or tagless payloads must not report ok");
    QVERIFY2(r.assets.isEmpty(), "a failed parse must not leak partial assets");
}

void UpdateInfoTest::parseRelease_skipsAssetsWithoutName()
{
    // A nameless asset can't be matched or saved to disk, so it is dropped
    // rather than carried through as an empty row.
    // Adjacent single-line raw literals — see the moc note above the fixtures.
    const Release r = parseRelease(QByteArrayLiteral(
        "{"
        "  \"tag_name\": \"0.2.3\","
        "  \"html_url\": \"https://github.com/logos-co/logos-basecamp/releases/tag/0.2.3\","
        "  \"assets\": ["
        "    {\"browser_download_url\": \"https://example.com/x\", \"size\": 10, \"state\": \"uploaded\"},"
        "    {\"name\": \"LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg\","
        "     \"browser_download_url\": \"https://example.com/y\", \"size\": 98701839,"
        "     \"state\": \"uploaded\"}"
        "  ]"
        "}"));

    QVERIFY(r.ok);
    QCOMPARE(r.assets.size(), 1);
    QCOMPARE(r.assets.at(0).name, QString::fromLatin1(kAarch64Dmg));
}

// ── transport safety ────────────────────────────────────────────────────────

void UpdateInfoTest::isTrustedReleaseUrl_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("expected");

    QTest::newRow("real release page")
        << "https://github.com/logos-co/logos-basecamp/releases/tag/0.2.3" << true;
    QTest::newRow("real download url")
        << "https://github.com/logos-co/logos-basecamp/releases/download/0.2.3/"
           "LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg" << true;

    // A fork (or a typosquat org) is not us.
    QTest::newRow("wrong org")
        << "https://github.com/evil/logos-basecamp/releases/tag/0.2.3" << false;
    QTest::newRow("wrong repo")
        << "https://github.com/logos-co/logos-basecamp-evil/releases/tag/0.2.3" << false;

    // Plain http is never acceptable — the payload is an executable.
    QTest::newRow("http downgrade")
        << "http://github.com/logos-co/logos-basecamp/releases/tag/0.2.3" << false;

    QTest::newRow("host-prefix spoof")
        << "https://github.com.evil.com/logos-co/logos-basecamp/releases/tag/0.2.3"
        << false;
    QTest::newRow("empty") << QString() << false;
}

void UpdateInfoTest::isTrustedReleaseUrl()
{
    QFETCH(QString, url);
    QFETCH(bool, expected);
    QCOMPARE(UpdateInfo::isTrustedReleaseUrl(url), expected);
}

void UpdateInfoTest::isAllowedDownloadHost_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<bool>("expected");

    // GitHub has moved release-asset hosting more than once, so the allowlist
    // covers the family rather than the host of the day.
    QTest::newRow("github.com")        << "github.com" << true;
    QTest::newRow("release-assets")    << "release-assets.githubusercontent.com" << true;
    QTest::newRow("objects")           << "objects.githubusercontent.com" << true;
    QTest::newRow("s3")                << "github-production-release-asset-2e65be.s3.amazonaws.com"
                                       << true;
    QTest::newRow("uppercase host")    << "GitHub.com" << true;

    QTest::newRow("evil.com")          << "evil.com" << false;
    // Suffix-confusion: the allowlisted string appears, but not as the suffix.
    QTest::newRow("suffix confusion")  << "githubusercontent.com.evil.com" << false;
    QTest::newRow("notgithub.com")     << "notgithub.com" << false;
    QTest::newRow("github.com.evil")   << "github.com.evil.com" << false;
    QTest::newRow("bare s3")           << "s3.amazonaws.com" << false;
    QTest::newRow("empty")             << QString() << false;
}

void UpdateInfoTest::isAllowedDownloadHost()
{
    QFETCH(QString, host);
    QFETCH(bool, expected);
    QCOMPARE(UpdateInfo::isAllowedDownloadHost(host), expected);
}

// ── presentation ────────────────────────────────────────────────────────────

void UpdateInfoTest::formatBytes_data()
{
    QTest::addColumn<qint64>("bytes");
    QTest::addColumn<QString>("expected");

    // Decimal MB/GB (1000-based) — matches what GitHub and Finder report, so
    // "271.1 MB" in our dialog matches the number the user sees on the site.
    QTest::newRow("aarch64 AppImage") << Q_INT64_C(271104520) << "271.1 MB";
    QTest::newRow("aarch64 dmg")      << Q_INT64_C(98701839)  << "98.7 MB";
    QTest::newRow("x86_64 AppImage")  << Q_INT64_C(278084088) << "278.1 MB";
    QTest::newRow("exactly 1 MB")     << Q_INT64_C(1000000)   << "1.0 MB";
    QTest::newRow("1 GB")             << Q_INT64_C(1000000000) << "1.0 GB";
    QTest::newRow("1 KB")             << Q_INT64_C(1000)      << "1 KB";
    QTest::newRow("just under 1 KB")  << Q_INT64_C(999)       << "999 B";
    QTest::newRow("zero")             << Q_INT64_C(0)         << "0 B";
    // Negative is "unknown", not "-0 B" — Content-Length is optional.
    QTest::newRow("negative")         << Q_INT64_C(-1)        << QString();
}

void UpdateInfoTest::formatBytes()
{
    QFETCH(qint64, bytes);
    QFETCH(QString, expected);
    QCOMPARE(UpdateInfo::formatBytes(bytes), expected);
}

void UpdateInfoTest::progressText_showsBothSizesAndPercent()
{
    const qint64 total    = 271104520;   // the real aarch64 AppImage
    const qint64 received = 135552260;   // exactly half

    const QString text = progressText(received, total);

    QVERIFY2(text.contains(QStringLiteral("135.6 MB")),
             qPrintable(QStringLiteral("received size missing from: ") + text));
    QVERIFY2(text.contains(QStringLiteral("271.1 MB")),
             qPrintable(QStringLiteral("total size missing from: ") + text));
    QVERIFY2(text.contains(QStringLiteral("50%")),
             qPrintable(QStringLiteral("percentage missing from: ") + text));

    // Exact form, with the separator spelled by code point so this assertion
    // does not depend on the source file's encoding surviving a round-trip.
    const QString expected = QStringLiteral("135.6 MB / 271.1 MB ")
                           + QChar(0x00B7) + QStringLiteral(" 50%");
    QCOMPARE(text, expected);

    // Integer truncation, not rounding: 99.9% must not read as 100%.
    QVERIFY2(progressText(total - 1, total).contains(QStringLiteral("99%")),
             "percentage truncates toward zero");
    QCOMPARE(progressText(0, total),
             QStringLiteral("0 B / 271.1 MB ") + QChar(0x00B7) + QStringLiteral(" 0%"));
    QVERIFY2(progressText(total, total).contains(QStringLiteral("100%")),
             "a completed download reads 100%");

    // Clamped at 100%. A server is free to send more bytes than its own
    // Content-Length advertised (or send none and let us fall back to the
    // catalog's size, which can disagree); the progress line must not then
    // read "113%". UpdateChecker also rejects a size mismatch outright, but
    // the label is rendered live during the transfer, before that check runs.
    QVERIFY2(progressText(total + 1, total).contains(QStringLiteral("100%")),
             "overshooting the advertised total still reads 100%");
    QVERIFY2(!progressText(total * 2, total).contains(QStringLiteral("200%")),
             "a wildly oversized body must not render a >100% percentage");
}

void UpdateInfoTest::progressText_unknownTotalFallsBackToReceivedOnly()
{
    // No Content-Length (chunked response): show what we have rather than a
    // bogus percentage or a divide-by-zero.
    QCOMPARE(progressText(1500000, 0),  QStringLiteral("1.5 MB"));
    QCOMPARE(progressText(1500000, -1), QStringLiteral("1.5 MB"));
}

void UpdateInfoTest::installHint_data()
{
    QTest::addColumn<QString>("os");
    QTest::addColumn<bool>("expectHint");

    QTest::newRow("macos")   << "macos"   << true;
    QTest::newRow("linux")   << "linux"   << true;
    QTest::newRow("windows") << "windows" << false;
    QTest::newRow("empty")   << QString() << false;
    // Case matters — hostOs() only ever emits the lowercase spellings.
    QTest::newRow("macOS mixed case") << "macOS" << false;
}

void UpdateInfoTest::installHint()
{
    QFETCH(QString, os);
    QFETCH(bool, expectHint);

    const QString fileName = QString::fromLatin1(kAarch64Dmg);
    const QString hint = UpdateInfo::installHint(os, fileName);

    if (!expectHint) {
        QVERIFY2(hint.isEmpty(),
                 qPrintable(QStringLiteral("unsupported OS must yield no hint, got: ") + hint));
        return;
    }

    QVERIFY2(!hint.isEmpty(), "supported OS must yield a hint");
    QVERIFY2(hint.contains(fileName),
             qPrintable(QStringLiteral("hint must name the downloaded file, got: ") + hint));
    // Leading instruction is load-bearing: replacing a running .app is what
    // produces the "application is damaged" reports.
    QVERIFY2(hint.startsWith(QStringLiteral("Quit Basecamp")),
             qPrintable(QStringLiteral("hint must lead with quitting the app, got: ") + hint));
}

// ── download decisions ──────────────────────────────────────────────────────
//
// These were inline in UpdateChecker.cpp's network callbacks, where they could
// not be reached from a unit test: tests/CMakeLists.txt links no Qt6::Network,
// so anything touching QNetworkReply fails to compile. Pulling the DECISION out
// of the I/O leaves UpdateChecker a thin shell and makes the rules testable.

void UpdateInfoTest::isAllowedDownloadUrl_requiresHttpsAndAllowedHost_data()
{
    QTest::addColumn<QString>("scheme");
    QTest::addColumn<QString>("host");
    QTest::addColumn<bool>("allowed");

    // The real redirect chain: github.com 302s to a signed CDN URL.
    QTest::newRow("github https")   << "https" << "github.com" << true;
    QTest::newRow("release CDN")    << "https" << "release-assets.githubusercontent.com" << true;
    QTest::newRow("objects CDN")    << "https" << "objects.githubusercontent.com" << true;
    QTest::newRow("s3 release bucket")
        << "https" << "github-production-release-asset-2e65be.s3.amazonaws.com" << true;
    // Self-service bucket name — must NOT satisfy the gate.
    QTest::newRow("s3 attacker bucket")
        << "https" << "attacker.s3.amazonaws.com" << false;
    QTest::newRow("scheme upcased") << "HTTPS" << "github.com" << true;

    // Downgrade to plaintext on a hop that ends in an executable. Qt's
    // NoLessSafeRedirectPolicy should already stop this; belt and braces.
    QTest::newRow("http downgrade") << "http"  << "github.com" << false;
    QTest::newRow("file scheme")    << "file"  << "github.com" << false;
    // Suffix-confusion: a host that merely ENDS with our domain as a label.
    QTest::newRow("suffix attack")  << "https" << "githubusercontent.com.evil.com" << false;
    QTest::newRow("lookalike")      << "https" << "notgithub.com" << false;
    QTest::newRow("unrelated")      << "https" << "evil.com" << false;
}

void UpdateInfoTest::isAllowedDownloadUrl_requiresHttpsAndAllowedHost()
{
    QFETCH(QString, scheme);
    QFETCH(QString, host);
    QFETCH(bool, allowed);
    QCOMPARE(UpdateInfo::isAllowedDownloadUrl(scheme, host), allowed);
}

void UpdateInfoTest::isAcceptableHttpStatus_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<bool>("ok");

    QTest::newRow("200 OK")        << 200 << true;
    // 0 = attribute absent. The UI tests point the check at a file:// stub,
    // which carries no HTTP status; rejecting 0 would break them.
    QTest::newRow("non-HTTP (0)")  << 0   << true;
    QTest::newRow("301")           << 301 << true;
    QTest::newRow("302 to CDN")    << 302 << true;
    QTest::newRow("307")           << 307 << true;

    // The one that matters: without this check a 404 HTML body gets written to
    // disk under a .dmg name and handed to the user as an installer.
    QTest::newRow("404")           << 404 << false;
    QTest::newRow("403 rate limit")<< 403 << false;
    QTest::newRow("500")           << 500 << false;
    QTest::newRow("418")           << 418 << false;
}

void UpdateInfoTest::isAcceptableHttpStatus()
{
    QFETCH(int, status);
    QFETCH(bool, ok);
    QCOMPARE(UpdateInfo::isAcceptableHttpStatus(status), ok);
}

void UpdateInfoTest::isDeclaredSizeMismatch_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<qint64>("declared");
    QTest::addColumn<qint64>("expected");
    QTest::addColumn<bool>("encoded");
    QTest::addColumn<bool>("mismatch");

    const qint64 dmg = Q_INT64_C(98701839);

    QTest::newRow("exact match")   << 200 << dmg     << dmg << false << false;
    QTest::newRow("swapped asset") << 200 << dmg + 1 << dmg << false << true;
    QTest::newRow("truncated")     << 200 << Q_INT64_C(100) << dmg << false << true;

    // A 3xx body is the redirect stub, not the asset — comparing it would
    // abort every download that follows a CDN redirect.
    QTest::newRow("redirect body") << 302 << Q_INT64_C(0) << dmg << false << false;
    // A compressed body legitimately differs from the on-disk asset size.
    QTest::newRow("gzipped")       << 200 << Q_INT64_C(5) << dmg << true  << false;
    // Nothing to compare against — degrade to TLS-only trust, don't false-alarm.
    QTest::newRow("no header")     << 200 << Q_INT64_C(-1) << dmg << false << false;
    QTest::newRow("unknown asset") << 200 << dmg << Q_INT64_C(0) << false << false;
}

void UpdateInfoTest::isDeclaredSizeMismatch()
{
    QFETCH(int, status);
    QFETCH(qint64, declared);
    QFETCH(qint64, expected);
    QFETCH(bool, encoded);
    QFETCH(bool, mismatch);
    QCOMPARE(UpdateInfo::isDeclaredSizeMismatch(status, declared, expected, encoded), mismatch);
}

void UpdateInfoTest::isCompleteDownload_data()
{
    QTest::addColumn<qint64>("written");
    QTest::addColumn<qint64>("expected");
    QTest::addColumn<bool>("complete");

    const qint64 dmg = Q_INT64_C(98701839);

    QTest::newRow("byte exact")      << dmg     << dmg << true;
    QTest::newRow("one byte short")  << dmg - 1 << dmg << false;
    QTest::newRow("overshoot")       << dmg + 1 << dmg << false;
    QTest::newRow("empty file")      << Q_INT64_C(0) << dmg << false;
    // Unknown published size: can't verify, so don't block the rename.
    QTest::newRow("unknown size")    << dmg << Q_INT64_C(0)  << true;
    QTest::newRow("negative size")   << dmg << Q_INT64_C(-1) << true;
}

void UpdateInfoTest::isCompleteDownload()
{
    QFETCH(qint64, written);
    QFETCH(qint64, expected);
    QFETCH(bool, complete);
    QCOMPARE(UpdateInfo::isCompleteDownload(written, expected), complete);
}

void UpdateInfoTest::hasEnoughSpaceFor_data()
{
    QTest::addColumn<qint64>("assetSize");
    QTest::addColumn<qint64>("available");
    QTest::addColumn<bool>("enough");

    const qint64 appImage = Q_INT64_C(271104520);          // the real aarch64 AppImage
    const qint64 headroom = UpdateInfo::kDiskHeadroomBytes;

    QTest::newRow("plenty free")   << appImage << appImage + headroom + 1 << true;
    QTest::newRow("exactly enough")<< appImage << appImage + headroom     << true;
    // Fits the file but not the headroom — refuse rather than fill the volume.
    QTest::newRow("no headroom")   << appImage << appImage + headroom - 1 << false;
    QTest::newRow("barely fits")   << appImage << appImage                << false;
    QTest::newRow("nearly full")   << appImage << Q_INT64_C(1024)         << false;
    // Unknown asset size — nothing to pre-check, let the write find out.
    QTest::newRow("unknown size")  << Q_INT64_C(0) << Q_INT64_C(0)        << true;
}

void UpdateInfoTest::hasEnoughSpaceFor()
{
    QFETCH(qint64, assetSize);
    QFETCH(qint64, available);
    QFETCH(bool, enough);
    QCOMPARE(UpdateInfo::hasEnoughSpaceFor(assetSize, available), enough);
}

void UpdateInfoTest::dedupedFileName_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<int>("index");
    QTest::addColumn<QString>("expected");

    // The real asset name is dot-heavy (v0.2.3). completeBaseName/suffix split
    // on the LAST dot, so the version must survive intact and only the true
    // extension moves — this is the case a naive split('.') gets wrong.
    QTest::newRow("real dmg")
        << "LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg" << 1
        << "LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64 (1).dmg";
    QTest::newRow("real AppImage")
        << "LogosBasecamp-Desktop-v0.2.3-aa2377-x86_64.AppImage" << 2
        << "LogosBasecamp-Desktop-v0.2.3-aa2377-x86_64 (2).AppImage";
    QTest::newRow("no extension") << "LogosBasecamp" << 1 << "LogosBasecamp (1)";
    QTest::newRow("index 50")     << "a.dmg" << 50 << "a (50).dmg";
    // Degenerate input must not produce " (1)" with an empty stem.
    QTest::newRow("empty")        << "" << 1 << "";
}

void UpdateInfoTest::dedupedFileName()
{
    QFETCH(QString, input);
    QFETCH(int, index);
    QFETCH(QString, expected);
    QCOMPARE(UpdateInfo::dedupedFileName(input, index), expected);
}

// ── security regressions ────────────────────────────────────────────────────
//
// Both cases below were found by the /ship security review and are real: the
// asset name and the asset URL both arrive from the network, and both were
// reaching the filesystem / the socket with only partial validation.

void UpdateInfoTest::isSafeAssetFileName_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("safe");

    QTest::newRow("real dmg")
        << "LogosBasecamp-Desktop-v0.2.3-aa2377-aarch64.dmg" << true;
    QTest::newRow("real AppImage")
        << "LogosBasecamp-Desktop-v0.2.3-aa2377-x86_64.AppImage" << true;

    // The exploit shape: satisfies selectAssetName's prefix AND suffix checks,
    // and QDir::filePath is a plain join — so this escapes ~/Downloads, and on
    // Linux the result would then be chmod +x'd.
    QTest::newRow("traversal")
        << "LogosBasecamp-Desktop-v9.9.9/../../../../tmp/pwn-aarch64.dmg" << false;
    QTest::newRow("absolute path")
        << "/etc/cron.d/pwn-aarch64.dmg" << false;
    QTest::newRow("backslash")
        << "LogosBasecamp-Desktop-v9.9.9\\..\\pwn-aarch64.dmg" << false;
    QTest::newRow("bare dotdot")   << ".." << false;
    QTest::newRow("dotfile")       << ".bashrc" << false;
    // QFile::encodeName truncates at NUL, so the path QFile opens would differ
    // from the one handed to setxattr.
    QTest::newRow("embedded NUL")
        << QString("LogosBasecamp-Desktop-v1") + QChar(QChar::Null) + QString("-aarch64.dmg")
        << false;
    QTest::newRow("newline")
        << "LogosBasecamp-Desktop-v1\n-aarch64.dmg" << false;
    QTest::newRow("empty")         << "" << false;
    QTest::newRow("overlong")      << QString(300, QLatin1Char('a')) << false;
}

void UpdateInfoTest::isSafeAssetFileName()
{
    QFETCH(QString, name);
    QFETCH(bool, safe);
    QCOMPARE(UpdateInfo::isSafeAssetFileName(name), safe);
}

void UpdateInfoTest::selectAssetName_rejectsPathTraversalDespiteMatchingPrefixAndSuffix()
{
    // Pin the ACTUAL regression, not just the helper: a traversal name that
    // passes both the prefix and suffix checks must still not be selected.
    Asset evil;
    evil.name  = QStringLiteral("LogosBasecamp-Desktop-v9.9.9/../../../../tmp/pwn-aarch64.dmg");
    evil.url   = QStringLiteral("https://github.com/logos-co/logos-basecamp/releases/download/x/e");
    evil.size  = 10;
    evil.state = QStringLiteral("uploaded");

    QVERIFY2(evil.name.startsWith(QStringLiteral("LogosBasecamp-Desktop-v")),
             "fixture must satisfy the prefix check, or it proves nothing");
    QVERIFY2(evil.name.endsWith(QStringLiteral("-aarch64.dmg")),
             "fixture must satisfy the suffix check, or it proves nothing");

    QCOMPARE(selectAssetName({evil}, QStringLiteral("macos"), QStringLiteral("arm64")),
             QString());
}

void UpdateInfoTest::isAllowedDownloadHost_rejectsArbitraryS3Buckets()
{
    // S3 bucket names are self-service, so a bare "*.s3.amazonaws.com" would let
    // anyone stand up a host that satisfies the only gate in front of a 271 MB
    // executable. Only GitHub's release-asset bucket family is accepted.
    QVERIFY(UpdateInfo::isAllowedDownloadHost(
        QStringLiteral("github-production-release-asset-2e65be.s3.amazonaws.com")));
    QVERIFY(!UpdateInfo::isAllowedDownloadHost(QStringLiteral("attacker.s3.amazonaws.com")));
    QVERIFY(!UpdateInfo::isAllowedDownloadHost(QStringLiteral("s3.amazonaws.com")));
}

void UpdateInfoTest::shouldSkipCheck_data()
{
    QTest::addColumn<bool>("disabled");
    QTest::addColumn<QString>("version");
    QTest::addColumn<bool>("forced");
    QTest::addColumn<bool>("portable");
    QTest::addColumn<bool>("skip");

    QTest::newRow("released portable build")  << false << "0.2.1" << false << true  << false;
    // A dev nix build has no AppImage/DMG to replace.
    QTest::newRow("released non-portable")    << false << "0.2.1" << false << false << true;
    // The rule the UI tests depend on: forcing a version waives portable-build.
    QTest::newRow("override waives portable") << false << "0.0.1" << true  << false << false;
    // Master CI builds are stamped pre-release-<sha7>; nothing to compare.
    QTest::newRow("dev version skips")        << false << "pre-release-aa23776" << false << true << true;
    QTest::newRow("empty version skips")      << false << "" << false << true << true;
    // Kill switch beats everything, including an explicit override.
    QTest::newRow("kill switch wins")         << true  << "0.2.1" << true  << true  << true;
}

void UpdateInfoTest::shouldSkipCheck()
{
    QFETCH(bool, disabled);
    QFETCH(QString, version);
    QFETCH(bool, forced);
    QFETCH(bool, portable);
    QFETCH(bool, skip);
    QCOMPARE(UpdateInfo::shouldSkipCheck(disabled, version, forced, portable), skip);
}

void UpdateInfoTest::downloadFraction_data()
{
    QTest::addColumn<qint64>("received");
    QTest::addColumn<qint64>("total");
    QTest::addColumn<qint64>("assetSize");
    QTest::addColumn<qreal>("expected");

    const qint64 t = Q_INT64_C(271104520);

    QTest::newRow("half")            << t / 2 << t << t << 0.5;
    QTest::newRow("start")           << Q_INT64_C(0) << t << t << 0.0;
    QTest::newRow("complete")        << t << t << t << 1.0;
    // Must agree with progressText's clamp: a bar past 1.0 next to a "100%"
    // label is the disagreement a user actually sees.
    QTest::newRow("overshoot clamps")<< t * 2 << t << t << 1.0;
    // No Content-Length: fall back to the size the release API published.
    QTest::newRow("falls back to asset size") << t / 2 << Q_INT64_C(-1) << t << 0.5;
    // Nothing to divide by -> indeterminate, not a division by zero.
    QTest::newRow("indeterminate")   << Q_INT64_C(10) << Q_INT64_C(0) << Q_INT64_C(0) << -1.0;
}

void UpdateInfoTest::downloadFraction()
{
    QFETCH(qint64, received);
    QFETCH(qint64, total);
    QFETCH(qint64, assetSize);
    QFETCH(qreal, expected);
    QCOMPARE(UpdateInfo::downloadFraction(received, total, assetSize), expected);
}

QTEST_GUILESS_MAIN(UpdateInfoTest)
#include "update_info_test.moc"
