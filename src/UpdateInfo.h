#pragma once

// Pure logic behind the host-app update check (issue #319).
//
// Everything here is a free function over plain values — no QNetworkAccessManager,
// no BuildInfo.h, no Qt GUI. That is deliberate: `tests/CMakeLists.txt` links only
// Core/Qml/Test/Widgets/Quick/QuickWidgets and does NOT put `app/utils` on the include
// path, so a unit test that pulled in either of those would fail to build. Keeping the
// interesting logic here means UpdateChecker is a thin I/O shell around tested code.
//
// Header-only for the same reason: no `// srcdeps:` line is needed in the test.

#include <QByteArray>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <logos/semver.hpp>

namespace UpdateInfo {

// One entry from the release's `assets[]`.
struct Asset {
    QString name;
    QString url;    // browser_download_url
    qint64  size = 0;
    QString state;  // "uploaded" once GitHub has finished ingesting it
};

struct Release {
    bool         ok = false;
    QString      tag;      // raw tag_name, e.g. "0.2.3"
    QString      htmlUrl;
    QList<Asset> assets;
};

// ── version handling ────────────────────────────────────────────────────────

// Release tags are bare ("0.2.3") but asset filenames carry a `v`
// ("…-v0.2.3-aa2377-…"). Strict semver rejects the prefix, so peel it first.
inline QString normalizeVersion(const QString& v)
{
    QString s = v.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V')))
        s = s.mid(1);
    return s;
}

// Is the *running* build something we can meaningfully compare?
//
// Load-bearing. `logos::semver::compare` sorts unparseable strings BELOW every
// parseable one, so without this gate `compare("pre-release-aa23776", "0.2.3")`
// returns -1 and every dev build would be told it is out of date. VERSION is only
// checked in on release/** branches; master CI builds get "pre-release-<sha7>" and
// dirty local builds get "".
inline bool isComparableVersion(const QString& current)
{
    return logos::semver::valid(normalizeVersion(current).toStdString());
}

// The single decision that determines whether this whole feature runs.
//
// Extracted from UpdateChecker's constructor because the composition is subtler
// than any of its parts: a non-empty LOGOS_UPDATE_CURRENT_VERSION (`forced`)
// WAIVES the portable-build requirement, which is what lets the UI tests drive
// the feature from a dev build. If that rule regresses the tests fail with an
// opaque 20s timeout rather than a named assertion.
inline bool shouldSkipCheck(bool disabled, const QString& currentVersion,
                            bool versionForced, bool portableBuild)
{
    if (disabled) return true;
    // Dev/pre-release builds have nothing comparable to compare against.
    if (!isComparableVersion(currentVersion)) return true;
    // A non-portable build has no AppImage/DMG to replace, so offering one is
    // misleading — unless the version was explicitly forced for testing.
    return !versionForced && !portableBuild;
}

// Strictly newer, never a downgrade.
//
// `/releases/latest` is ordered by creation date, NOT semver precedence, so a
// hotfix cut from an old branch can legitimately be "latest". Requiring a strict
// `>` means we stay quiet in that case rather than offering a downgrade.
inline bool isNewer(const QString& current, const QString& latest)
{
    const QString cur = normalizeVersion(current);
    const QString lat = normalizeVersion(latest);
    if (!logos::semver::valid(cur.toStdString()))
        return false;
    // The fetched tag is checked leniently: it is upstream data, and `1.2` or
    // `v1.2.3` are unambiguous even though strict semver rejects them.
    if (!logos::semver::parse_lenient(lat.toStdString()).has_value())
        return false;
    return logos::semver::compare(lat.toStdString(), cur.toStdString()) > 0;
}

// ── release payload ─────────────────────────────────────────────────────────

inline Release parseRelease(const QByteArray& json)
{
    Release r;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return r;

    const QJsonObject obj = doc.object();
    r.tag     = obj.value(QStringLiteral("tag_name")).toString();
    r.htmlUrl = obj.value(QStringLiteral("html_url")).toString();
    // Both are required. html_url in particular: the caller gates trust on it
    // (isTrustedReleaseUrl), so treating an absent one as acceptable would let a
    // payload skip that check simply by omitting the field — failing open.
    if (r.tag.isEmpty() || r.htmlUrl.isEmpty())
        return r;

    for (const QJsonValue& v : obj.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        Asset asset;
        asset.name  = a.value(QStringLiteral("name")).toString();
        asset.url   = a.value(QStringLiteral("browser_download_url")).toString();
        asset.size  = static_cast<qint64>(a.value(QStringLiteral("size")).toDouble());
        asset.state = a.value(QStringLiteral("state")).toString();
        if (!asset.name.isEmpty())
            r.assets.append(asset);
    }

    r.ok = true;
    return r;
}

// Only ever trust a release page that lives on our own repo.
inline bool isTrustedReleaseUrl(const QString& url)
{
    return url.startsWith(QStringLiteral("https://github.com/logos-co/logos-basecamp/"));
}

// ── asset selection ─────────────────────────────────────────────────────────

// The single most likely way this feature ships silently broken:
// QSysInfo::currentCpuArchitecture() returns "arm64", while the release assets
// are named "-aarch64". Map explicitly and return empty for anything unknown so
// the caller degrades to the link-out rather than guessing.
inline QString archToken(const QString& cpuArchitecture)
{
    const QString a = cpuArchitecture.toLower();
    if (a == QStringLiteral("arm64") || a == QStringLiteral("aarch64"))
        return QStringLiteral("aarch64");
    if (a == QStringLiteral("x86_64") || a == QStringLiteral("amd64"))
        return QStringLiteral("x86_64");
    return QString();
}

// "macos" → .dmg, "linux" → .AppImage. Anything else is unsupported.
inline QString assetExtension(const QString& os)
{
    if (os == QStringLiteral("macos")) return QStringLiteral("dmg");
    if (os == QStringLiteral("linux")) return QStringLiteral("AppImage");
    return QString();
}

// The asset name arrives from the network and is used to build a path under the
// user's Downloads directory, so it must be a bare filename — nothing that can
// climb out of that directory or desynchronise the path seen by different APIs.
//
// The prefix/suffix checks in selectAssetName are NOT sufficient on their own:
// "LogosBasecamp-Desktop-v9.9.9/../../../../tmp/pwn-aarch64.dmg" satisfies both,
// and QDir::filePath is a plain join. On Linux the result would then be chmod
// +x'd. Embedded NUL/newline are rejected too: QFile::encodeName truncates at
// NUL, so the path QFile opens could differ from the one passed to setxattr.
inline bool isSafeAssetFileName(const QString& name)
{
    if (name.isEmpty() || name.size() > 255)
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    if (name.contains(QStringLiteral("..")))
        return false;
    if (name.startsWith(QLatin1Char('.')))       // no dotfiles
        return false;
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F)
            return false;
    }
    // Belt and braces: the name must already be its own leaf.
    return QFileInfo(name).fileName() == name;
}

// Assets are named LogosBasecamp-Desktop-v<VERSION>-<sha6>-<arch>.<ext>. Never
// match on the version or the short hash — the hash is unpredictable. Never match
// on content_type either: all three assets report application/octet-stream.
//
// Returns an empty name when nothing fits (Intel Mac has no x86_64 dmg, Windows
// has nothing at all). That is a UI state, not an error.
inline QString selectAssetName(const QList<Asset>& assets,
                               const QString& os,
                               const QString& cpuArchitecture)
{
    const QString arch = archToken(cpuArchitecture);
    const QString ext  = assetExtension(os);
    if (arch.isEmpty() || ext.isEmpty())
        return QString();

    const QString suffix = QStringLiteral("-%1.%2").arg(arch, ext);
    for (const Asset& a : assets) {
        // A half-uploaded asset reports state "starting"; downloading it yields a
        // truncated file.
        if (!a.state.isEmpty() && a.state != QStringLiteral("uploaded"))
            continue;
        if (!isSafeAssetFileName(a.name))
            continue;
        if (!a.name.startsWith(QStringLiteral("LogosBasecamp-Desktop-v")))
            continue;
        if (a.name.endsWith(suffix, Qt::CaseInsensitive))
            return a.name;
    }
    return QString();
}

// ── transport safety ────────────────────────────────────────────────────────

// GitHub has served release assets from objects.githubusercontent.com, S3, and
// today release-assets.githubusercontent.com. Allow the family rather than the
// single host of the day, or this breaks the next time they rotate CDNs.
//
// The leading dot matters: it defeats "evil-githubusercontent.com", while
// endsWith defeats "githubusercontent.com.evil.com".
//
// S3 is scoped to GitHub's own release-asset bucket family. A bare
// "*.s3.amazonaws.com" would admit ANY bucket, and bucket names are
// self-service — an attacker could register one and satisfy the only host gate
// standing between the user and a 271 MB executable.
inline bool isAllowedDownloadHost(const QString& host)
{
    const QString h = host.toLower();
    return h == QStringLiteral("github.com")
        || h.endsWith(QStringLiteral(".githubusercontent.com"))
        || (h.startsWith(QStringLiteral("github-production-release-asset"))
            && h.endsWith(QStringLiteral(".s3.amazonaws.com")));
}

// Scheme AND host. Qt's NoLessSafeRedirectPolicy already blocks https→http, but
// this hop ends in an executable the user is told to run, so don't rely on a
// single layer for that.
inline bool isAllowedDownloadUrl(const QString& scheme, const QString& host)
{
    return scheme.toLower() == QStringLiteral("https") && isAllowedDownloadHost(host);
}

// Accept the 200 we want, the 3xx we're about to follow, and 0 — which is what
// QNetworkRequest::HttpStatusCodeAttribute reports for a non-HTTP reply such as
// the file:// stub the UI tests point at.
inline bool isAcceptableHttpStatus(int status)
{
    if (status == 0 || status == 200) return true;
    return status >= 300 && status <= 399;
}

// Compare the server's declared Content-Length against the size the release API
// published. A zero-cost detector for a swapped artifact or an injecting proxy.
// Only meaningful on a final 200 with no transfer encoding: a 3xx body is the
// redirect stub, and a compressed body legitimately differs from the asset size.
inline bool isDeclaredSizeMismatch(int status, qint64 declaredLength,
                                   qint64 expectedSize, bool contentEncoded)
{
    if (status != 200 || contentEncoded) return false;
    if (declaredLength < 0 || expectedSize <= 0) return false;   // nothing to compare
    return declaredLength != expectedSize;
}

// Final gate before the .part file is renamed into place. An unknown expected
// size can't be checked, so it passes — TLS is the only guarantee there.
inline bool isCompleteDownload(qint64 bytesWritten, qint64 expectedSize)
{
    if (expectedSize <= 0) return true;
    return bytesWritten == expectedSize;
}

// ── destination naming / capacity ───────────────────────────────────────────

// Leave headroom so a download never fills the volume to the last byte.
inline constexpr qint64 kDiskHeadroomBytes = 64LL * 1024 * 1024;

inline bool hasEnoughSpaceFor(qint64 assetSize, qint64 bytesAvailable)
{
    if (assetSize <= 0) return true;   // unknown size — nothing to pre-check
    return bytesAvailable >= assetSize + kDiskHeadroomBytes;
}

// "foo.dmg" + 1 → "foo (1).dmg". Used to pick a free name rather than clobbering
// a file the user already has. Note completeBaseName/suffix split on the LAST
// dot, so the version dots in LogosBasecamp-Desktop-v0.2.3-... survive intact.
inline QString dedupedFileName(const QString& fileName, int index)
{
    const QFileInfo fi(fileName);
    const QString base = fi.completeBaseName();
    const QString ext  = fi.suffix();
    if (base.isEmpty()) return fileName;
    return ext.isEmpty()
        ? QStringLiteral("%1 (%2)").arg(base).arg(index)
        : QStringLiteral("%1 (%2).%3").arg(base).arg(index).arg(ext);
}

// ── presentation helpers (here so QML never does byte arithmetic) ───────────

inline QString formatBytes(qint64 bytes)
{
    if (bytes < 0) return QString();
    constexpr double kMB = 1000.0 * 1000.0;
    constexpr double kGB = kMB * 1000.0;
    if (bytes >= static_cast<qint64>(kGB))
        return QStringLiteral("%1 GB").arg(bytes / kGB, 0, 'f', 1);
    if (bytes >= static_cast<qint64>(kMB))
        return QStringLiteral("%1 MB").arg(bytes / kMB, 0, 'f', 1);
    if (bytes >= 1000)
        return QStringLiteral("%1 KB").arg(bytes / 1000.0, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

// Fraction for the progress bar, 0..1, or -1 when the total is unknown
// (indeterminate). Clamped for the same reason progressText is: a server may
// send more than it advertised, and a bar past 1.0 next to a label reading
// "100%" is the kind of disagreement that only shows up in front of a user.
inline qreal downloadFraction(qint64 received, qint64 total, qint64 assetSize)
{
    const qint64 denom = total > 0 ? total : assetSize;
    if (denom <= 0) return -1.0;
    const qreal f = static_cast<qreal>(received) / static_cast<qreal>(denom);
    return f > 1.0 ? 1.0 : (f < 0.0 ? 0.0 : f);
}

inline QString progressText(qint64 received, qint64 total)
{
    if (total <= 0)
        return formatBytes(received);
    // Clamped: a server that sends more than it advertised should not render
    // "113%".
    const int pct = static_cast<int>(qMin<qint64>(100, (received * 100) / total));
    return QStringLiteral("%1 / %2 · %3%")
        .arg(formatBytes(received), formatBytes(total))
        .arg(pct);
}

// Post-download instructions. Deliberately lead with "Quit Basecamp first" —
// the app cannot replace its own running binary, and replacing a running .app is
// exactly what produces "the application is damaged" reports.
inline QString installHint(const QString& os, const QString& fileName)
{
    if (os == QStringLiteral("macos")) {
        return QStringLiteral(
            "Quit Basecamp, then open %1 and drag LogosBasecamp onto Applications, "
            "replacing the existing copy. Eject the disk image when you're done.")
            .arg(fileName);
    }
    if (os == QStringLiteral("linux")) {
        return QStringLiteral(
            "Quit Basecamp, then run the new AppImage — it has already been made "
            "executable. Your existing AppImage is left untouched; delete or replace "
            "it once you're happy with %1.")
            .arg(fileName);
    }
    return QString();
}

// Compile-time host OS. Kept apart from selectAssetName() so the selection logic
// stays testable for every platform from any build host.
inline QString hostOs()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QString();
#endif
}

}  // namespace UpdateInfo
