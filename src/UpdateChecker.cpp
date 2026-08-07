#include "UpdateChecker.h"

#include "BuildInfo.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSslSocket>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_MACOS
#  include <sys/xattr.h>
#endif

namespace {

// GitHub's "latest" endpoint deliberately skips drafts and pre-releases, so it
// returns the stable tag (0.2.3) and never the CI `pre-release-<sha>` tags that
// .github/workflows/build.yml publishes.
constexpr auto kDefaultApiUrl =
    "https://api.github.com/repos/logos-co/logos-basecamp/releases/latest";

constexpr auto kFallbackReleasesUrl =
    "https://github.com/logos-co/logos-basecamp/releases/latest";

// Both are *idle* timeouts, not total deadlines — QNetworkRequest restarts the
// timer on every chunk. 10s of silence on a 10KB JSON body means something is
// wrong; 10s of silence mid-transfer is routine on tethering, and throwing away
// 250MB over a hiccup is a bad trade. No total deadline: a slow link legitimately
// takes an hour for a 271MB asset.
constexpr int kApiTimeoutMs      = 10 * 1000;
constexpr int kDownloadTimeoutMs = 60 * 1000;

// Bound Qt's in-memory buffer so TCP back-pressure kicks in when the disk is
// slower than the network. Without it Qt buffers the whole body in RAM.
constexpr qint64 kReadBufferBytes = 1LL << 20;

// Must not exceed kReadBufferBytes.
constexpr qint64 kChunkBytes = 256LL * 1024;

// Give up rather than overwrite once this many candidate names are taken.
constexpr int kMaxDedupAttempts = 50;

// Keeps the check off the module-load critical path, and clear of
// shutdown-tests.mjs, which quits the app ~2s after launch.
constexpr int kStartupCheckDelayMs = 5 * 1000;

bool updateCheckDisabled()
{
    return qEnvironmentVariableIsSet("LOGOS_DISABLE_UPDATE_CHECK");
}

QString apiUrl()
{
    // Test-only override so ui-tests can point at a local stub instead of the
    // live API. Never set in production.
    const QString override = qEnvironmentVariable("LOGOS_UPDATE_CHECK_URL");
    return override.isEmpty() ? QString::fromLatin1(kDefaultApiUrl) : override;
}

void applyCommonHeaders(QNetworkRequest& req, const QString& version)
{
    // Qt injects a Mozilla/5.0 UA when none is set and GitHub only rejects a
    // *missing* one, but identify ourselves properly anyway.
    req.setRawHeader("User-Agent",
                     QStringLiteral("LogosBasecamp/%1")
                         .arg(version.isEmpty() ? QStringLiteral("dev") : version)
                         .toUtf8());
    // Deliberately NO Authorization header. Qt copies headers verbatim across a
    // redirect, so it would leak to the CDN host — which then rejects the signed
    // URL it was given. This is a public repo; unauthenticated GET is correct.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(5);
}

}  // namespace

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
{
    // Test-only override of the "installed" side of the comparison, so the
    // feature can be exercised without cutting a release. Setting it also waives
    // the portable-build requirement below — that is the entire point of it.
    const QString versionOverride = qEnvironmentVariable("LOGOS_UPDATE_CURRENT_VERSION");
    const bool    forced          = !versionOverride.isEmpty();
    m_currentVersion = forced ? versionOverride : LogosBasecampBuildInfo::version();

    // Skip silently — and make no network request at all — unless this build is
    // something we can meaningfully compare and offer an artifact for:
    //
    //  * VERSION is only checked in on release/** branches. Master CI builds are
    //    stamped "pre-release-<sha7>" and dirty local builds get "", neither of
    //    which is semver. Without this gate logos::semver::compare would sort the
    //    unparseable string BELOW every real version and tell every developer
    //    they were out of date.
    //  * A non-portable nix build has no AppImage/DMG to replace, so offering a
    //    download would be misleading.
    if (UpdateInfo::shouldSkipCheck(updateCheckDisabled(), m_currentVersion,
                                    forced, LogosBasecampBuildInfo::isPortableBuild())) {
        m_checkState = UpdateCheckState::Skipped;
        return;
    }

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { abortAll(); });

    // Deferred so the check stays off the module-load critical path, mirroring
    // the deferred first catalog scan in MainUIBackend. Also keeps it clear of
    // shutdown-tests.mjs, which quits the app ~2s after launch.
    QTimer::singleShot(kStartupCheckDelayMs, this, &UpdateChecker::checkForUpdates);
}

UpdateChecker::~UpdateChecker()
{
    abortAll();
}

void UpdateChecker::abortAll()
{
    // Disconnect before aborting: abort() synthesizes finished(), and we must not
    // re-enter a half-destroyed object.
    if (m_checkReply) {
        m_checkReply->disconnect(this);
        m_checkReply->abort();
        m_checkReply->deleteLater();
        m_checkReply = nullptr;
    }
    if (m_downloadReply) {
        m_downloadReply->disconnect(this);
        m_downloadReply->abort();
        m_downloadReply->deleteLater();
        m_downloadReply = nullptr;
    }
    if (m_downloadFile) {
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile = nullptr;
    }
    // Never leave a partial multi-hundred-megabyte file behind.
    if (!m_partFilePath.isEmpty()) {
        QFile::remove(m_partFilePath);
        m_partFilePath.clear();
    }
}

void UpdateChecker::setCheckState(UpdateCheckState::Value s)
{
    if (m_checkState == s) return;
    m_checkState = s;
    emit checkStateChanged();
}

void UpdateChecker::setDownloadStage(UpdateDownloadStage::Value s)
{
    if (m_downloadStage == s) return;
    m_downloadStage = s;
    emit downloadStateChanged();
}

// ── version check ───────────────────────────────────────────────────────────

void UpdateChecker::checkForUpdates()
{
    if (m_checkState == UpdateCheckState::Skipped) return;
    if (m_checkReply) return;   // already in flight; QML buttons double-fire

    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
        // One-time diagnostics. This is the first code in a shipped bundle that
        // ever loads a Qt TLS backend, so if the plugin is missing from an
        // AppImage/DMG we want it in the session log rather than showing up as a
        // permanent silent "no update".
        qInfo().noquote() << QStringLiteral("UpdateChecker: TLS available=%1 backend=%2 (%3)")
                                 .arg(QSslSocket::supportsSsl() ? "yes" : "no",
                                      QSslSocket::activeBackend(),
                                      QSslSocket::sslLibraryVersionString());
    }

    setCheckState(UpdateCheckState::Checking);

    QNetworkRequest req{QUrl(apiUrl())};
    applyCommonHeaders(req, m_currentVersion);
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setTransferTimeout(kApiTimeoutMs);

    m_checkReply = m_nam->get(req);
    // The release JSON is a few KB. Bound it anyway: the URL is overridable via
    // LOGOS_UPDATE_CHECK_URL, and setTransferTimeout is an IDLE timeout, so a
    // server that streams slowly but continuously would never trip it while the
    // readAll() below grew the heap.
    m_checkReply->setReadBufferSize(64 * 1024);
    connect(m_checkReply, &QNetworkReply::finished, this, &UpdateChecker::onCheckFinished);
}

void UpdateChecker::onCheckFinished()
{
    QNetworkReply* reply = m_checkReply;
    if (!reply) return;
    m_checkReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        // Rate limiting (60/hr per IP, tight behind corporate NAT), a TLS
        // failure, DNS failure in a sandboxed nix build — all land here, and all
        // must read as "unknown", never as "you're on the latest".
        qInfo().noquote() << "UpdateChecker: check failed:" << reply->errorString();
        setCheckState(UpdateCheckState::Failed);
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!UpdateInfo::isAcceptableHttpStatus(status) && status != 404) {
        // Same policy as the download reply. Without this a 204, or a 200-with-
        // HTML from a captive portal, falls through to parseRelease untested.
        qInfo().noquote() << "UpdateChecker: check returned HTTP" << status;
        setCheckState(UpdateCheckState::Failed);
        return;
    }
    if (status == 404) {
        // Happens when the repo has no non-prerelease release at all. Since the
        // real releases are cut outside .github/workflows/build.yml (which only
        // ever publishes prerelease:true), this log line is the only clue if
        // someone forgets to un-check "pre-release" on a release.
        qWarning() << "UpdateChecker: /releases/latest returned 404 — is the newest "
                      "release still marked as a pre-release?";
        setCheckState(UpdateCheckState::Failed);
        return;
    }

    const UpdateInfo::Release rel = UpdateInfo::parseRelease(reply->readAll());
    if (!rel.ok) {
        qWarning() << "UpdateChecker: could not parse release payload";
        setCheckState(UpdateCheckState::Failed);
        return;
    }
    // Unconditional: parseRelease guarantees html_url is present, so there is no
    // "absent means fine" path through this gate.
    if (!UpdateInfo::isTrustedReleaseUrl(rel.htmlUrl)) {
        qWarning() << "UpdateChecker: release url is not on logos-basecamp:" << rel.htmlUrl;
        setCheckState(UpdateCheckState::Failed);
        return;
    }

    m_latestVersion = UpdateInfo::normalizeVersion(rel.tag);
    m_releaseUrl    = rel.htmlUrl;

    if (!UpdateInfo::isNewer(m_currentVersion, m_latestVersion)) {
        setCheckState(UpdateCheckState::UpToDate);
        return;
    }

    // Resolve the artifact for this platform. No match (Intel Mac has no x86_64
    // dmg; Windows has nothing) is a UI state, not an error — the notice still
    // shows, only the Download button is replaced by the link-out.
    const QString wanted = UpdateInfo::selectAssetName(
        rel.assets, UpdateInfo::hostOs(), QSysInfo::currentCpuArchitecture());
    m_assetUrl.clear();
    m_assetName.clear();
    m_assetSize = 0;
    for (const UpdateInfo::Asset& a : rel.assets) {
        if (a.name != wanted) continue;
        // Reject an off-allowlist asset URL here rather than at click time, so
        // the UI shows the release-page link instead of a Download button that
        // would fail. selectAssetName has already vetted the name itself.
        const QUrl assetUrl(a.url);
        if (!UpdateInfo::isAllowedDownloadUrl(assetUrl.scheme(), assetUrl.host())) {
            qWarning() << "UpdateChecker: release asset is hosted off-allowlist:" << a.url;
            break;
        }
        m_assetName = a.name;
        m_assetUrl  = a.url;
        m_assetSize = a.size;
        break;
    }

    if (m_assetUrl.isEmpty()) {
        qInfo().noquote() << QStringLiteral("UpdateChecker: %1 available, no asset for %2/%3")
                                 .arg(m_latestVersion, UpdateInfo::hostOs(),
                                      QSysInfo::currentCpuArchitecture());
        setDownloadStage(UpdateDownloadStage::Unsupported);
    } else {
        m_installHint = UpdateInfo::installHint(UpdateInfo::hostOs(), m_assetName);
        setDownloadStage(UpdateDownloadStage::Idle);
    }

    qInfo().noquote() << QStringLiteral("UpdateChecker: update available %1 → %2")
                             .arg(m_currentVersion, m_latestVersion);
    setCheckState(UpdateCheckState::UpdateAvailable);
}

// ── download ────────────────────────────────────────────────────────────────

void UpdateChecker::startDownload()
{
    if (m_downloadReply) {
        qWarning() << "UpdateChecker: download already in flight";
        return;
    }
    if (m_assetUrl.isEmpty()) return;

    // Gate the INITIAL url, not just the redirect hops. browser_download_url
    // comes from the release payload, so without this an asset entry pointing at
    // https://evil.example/payload.dmg would be fetched directly — the redirect
    // handler never sees it, because no redirect occurs.
    const QUrl url(m_assetUrl);
    if (!UpdateInfo::isAllowedDownloadUrl(url.scheme(), url.host())) {
        failDownload(QStringLiteral("Refusing to download from an untrusted host."));
        return;
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (dir.isEmpty()) {
        failDownload(QStringLiteral("Couldn't find a folder to download into."));
        return;
    }
    if (!QDir().mkpath(dir)) {
        failDownload(QStringLiteral("Couldn't create %1.").arg(dir));
        return;
    }

    // Defense in depth: selectAssetName already rejected anything that isn't a
    // bare filename, but m_assetName is network-derived and about to become a
    // filesystem path, so re-check at the point of use.
    if (!UpdateInfo::isSafeAssetFileName(m_assetName)) {
        failDownload(QStringLiteral("Release asset has an unsafe filename."));
        return;
    }

    // Already have exactly this artifact? Don't re-download 271 MB.
    const QString finalPath = QDir(dir).filePath(m_assetName);
    if (QFileInfo::exists(finalPath)
        && m_assetSize > 0
        && QFileInfo(finalPath).size() == m_assetSize) {
        m_downloadedFilePath = finalPath;
        m_downloadError.clear();
        setDownloadStage(UpdateDownloadStage::Done);
        return;
    }

    // Otherwise pick a free name rather than clobbering whatever is there.
    // Bail out rather than overwrite if every candidate is taken. Note this is
    // best-effort at start-of-download: finalizeDownload() does overwrite the
    // chosen target at rename time, so anything that lands there during the
    // (multi-minute) transfer is replaced.
    QString target = finalPath;
    for (int i = 1; i <= kMaxDedupAttempts && QFileInfo::exists(target); ++i)
        target = QDir(dir).filePath(UpdateInfo::dedupedFileName(m_assetName, i));
    if (QFileInfo::exists(target)) {
        failDownload(QStringLiteral("Too many copies of %1 already in %2.")
                         .arg(m_assetName, dir));
        return;
    }

    const QStorageInfo storage{dir};
    if (storage.isValid()
        && !UpdateInfo::hasEnoughSpaceFor(m_assetSize, storage.bytesAvailable())) {
        failDownload(QStringLiteral("Needs %1 free in %2; only %3 available.")
                         .arg(UpdateInfo::formatBytes(m_assetSize + UpdateInfo::kDiskHeadroomBytes),
                              dir,
                              UpdateInfo::formatBytes(storage.bytesAvailable())));
        return;
    }

    // Write to <final>.part and rename on success, so an interrupted download
    // never leaves something that looks like a usable installer.
    m_partFilePath = target + QStringLiteral(".part");
    QFile::remove(m_partFilePath);
    m_downloadFile = new QFile(m_partFilePath, this);
    if (!m_downloadFile->open(QIODevice::WriteOnly)) {
        const QString err = m_downloadFile->errorString();
        delete m_downloadFile;
        m_downloadFile = nullptr;
        m_partFilePath.clear();
        failDownload(QStringLiteral("Couldn't write to %1: %2").arg(target, err));
        return;
    }

    m_downloadedFilePath = target;
    m_downloadCancelled  = false;
    m_bytesWritten       = 0;
    m_downloadProgress   = -1.0;
    m_progressText.clear();
    m_downloadError.clear();
    setDownloadStage(UpdateDownloadStage::Starting);

    if (!m_nam) m_nam = new QNetworkAccessManager(this);

    QNetworkRequest req{url};
    applyCommonHeaders(req, m_currentVersion);
    req.setTransferTimeout(kDownloadTimeoutMs);

    m_downloadReply = m_nam->get(req);
    m_downloadReply->setReadBufferSize(kReadBufferBytes);

    // GitHub 302s to a signed CDN URL. Allow the githubusercontent/S3 family
    // rather than today's exact host, or this breaks the next time they rotate.
    connect(m_downloadReply, &QNetworkReply::redirected, this,
            [this](const QUrl& to) {
                if (!UpdateInfo::isAllowedDownloadUrl(to.scheme(), to.host())) {
                    qWarning() << "UpdateChecker: refusing redirect to" << to.toString();
                    m_downloadCancelled = true;
                    if (m_downloadReply) m_downloadReply->abort();
                }
            });

    // Validate before a single byte hits the disk — otherwise a 404 HTML page
    // gets happily written out under a .dmg name.
    connect(m_downloadReply, &QNetworkReply::metaDataChanged, this, [this] {
        if (!m_downloadReply) return;
        const int status =
            m_downloadReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!UpdateInfo::isAcceptableHttpStatus(status)) {
            m_downloadCancelled = true;
            m_downloadReply->abort();
            failDownload(QStringLiteral("Download failed (HTTP %1).").arg(status));
            return;
        }
        const QVariant len = m_downloadReply->header(QNetworkRequest::ContentLengthHeader);
        const bool encoded = !m_downloadReply->rawHeader("Content-Encoding").isEmpty();
        if (UpdateInfo::isDeclaredSizeMismatch(status,
                                               len.isValid() ? len.toLongLong() : -1,
                                               m_assetSize, encoded)) {
            m_downloadCancelled = true;
            m_downloadReply->abort();
            failDownload(QStringLiteral("Download size didn't match the published release."));
        }
    });

    connect(m_downloadReply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                const qint64 denom = total > 0 ? total : m_assetSize;
                m_downloadProgress = UpdateInfo::downloadFraction(received, total, m_assetSize);
                m_progressText     = UpdateInfo::progressText(received, denom);
                if (m_downloadStage != UpdateDownloadStage::Downloading)
                    setDownloadStage(UpdateDownloadStage::Downloading);
                else
                    emit downloadStateChanged();
            });

    connect(m_downloadReply, &QNetworkReply::readyRead, this,
            &UpdateChecker::onDownloadReadyRead);
    connect(m_downloadReply, &QNetworkReply::finished, this,
            &UpdateChecker::onDownloadFinished);
}

void UpdateChecker::onDownloadReadyRead()
{
    if (!m_downloadReply || !m_downloadFile) return;
    // Stream in chunks. Never readAll() — that would hold 271 MB resident.
    while (m_downloadReply->bytesAvailable() > 0) {
        const QByteArray chunk = m_downloadReply->read(kChunkBytes);
        if (chunk.isEmpty()) break;
        const qint64 written = m_downloadFile->write(chunk);
        if (written != chunk.size()) {
            m_downloadCancelled = true;
            m_downloadReply->abort();
            failDownload(QStringLiteral("Couldn't write to %1: %2")
                             .arg(m_downloadedFilePath, m_downloadFile->errorString()));
            return;
        }
        m_bytesWritten += written;
    }
}

void UpdateChecker::onDownloadFinished()
{
    QNetworkReply* reply = m_downloadReply;
    if (!reply) return;
    m_downloadReply = nullptr;
    reply->deleteLater();

    const bool cancelled = m_downloadCancelled;
    const QNetworkReply::NetworkError err = reply->error();

    if (cancelled) {
        // A deliberate user action is not a failure — go back to Idle, no red.
        // (failDownload() may already have set Failed for the abort-on-bad-header
        // paths; those set m_downloadError, so don't stomp them.)
        teardownDownload();
        if (m_downloadError.isEmpty())
            setDownloadStage(UpdateDownloadStage::Idle);
        return;
    }
    if (err != QNetworkReply::NoError) {
        teardownDownload();
        failDownload(reply->errorString());
        return;
    }

    finalizeDownload();
}

void UpdateChecker::finalizeDownload()
{
    if (!m_downloadFile) return;

    m_downloadFile->flush();
    m_downloadFile->close();
    const QString part = m_partFilePath;
    delete m_downloadFile;
    m_downloadFile = nullptr;

    if (!UpdateInfo::isCompleteDownload(m_bytesWritten, m_assetSize)) {
        QFile::remove(part);
        m_partFilePath.clear();
        failDownload(QStringLiteral("Download was incomplete (%1 of %2).")
                         .arg(UpdateInfo::formatBytes(m_bytesWritten),
                              UpdateInfo::formatBytes(m_assetSize)));
        return;
    }

    QFile::remove(m_downloadedFilePath);
    if (!QFile::rename(part, m_downloadedFilePath)) {
        QFile::remove(part);
        m_partFilePath.clear();
        failDownload(QStringLiteral("Couldn't save to %1.").arg(m_downloadedFilePath));
        return;
    }
    m_partFilePath.clear();

#ifdef Q_OS_LINUX
    // A Qt-written file is 0644, and a double-clicked non-executable AppImage
    // does nothing at all — silently.
    QFile::setPermissions(m_downloadedFilePath,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner
                              | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                              | QFileDevice::ExeGroup | QFileDevice::ReadOther
                              | QFileDevice::ExeOther);
#endif

#ifdef Q_OS_MACOS
    // Downloading it ourselves would otherwise REMOVE a safety net: Gatekeeper
    // only assesses a file that carries com.apple.quarantine, and QFile writes
    // none. The .app inside the DMG is Developer-ID signed, notarized and
    // stapled, so restoring the xattr gives the user the same one-time
    // "downloaded from the Internet" prompt a browser download would.
    const QByteArray path = QFile::encodeName(m_downloadedFilePath);
    const QByteArray value = QStringLiteral("0083;%1;LogosBasecamp;")
                                 .arg(QDateTime::currentSecsSinceEpoch(), 0, 16)
                                 .toUtf8();
    if (setxattr(path.constData(), "com.apple.quarantine",
                 value.constData(), static_cast<size_t>(value.size()), 0, 0) != 0) {
        qWarning() << "UpdateChecker: could not set quarantine attribute on"
                   << m_downloadedFilePath;
    }
#endif

    m_downloadError.clear();
    qInfo().noquote() << "UpdateChecker: saved" << m_downloadedFilePath;
    setDownloadStage(UpdateDownloadStage::Done);
}

void UpdateChecker::teardownDownload()
{
    if (m_downloadFile) {
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile = nullptr;
    }
    if (!m_partFilePath.isEmpty()) {
        QFile::remove(m_partFilePath);
        m_partFilePath.clear();
    }
}

void UpdateChecker::failDownload(const QString& message)
{
    m_downloadError = message;
    qWarning().noquote() << "UpdateChecker: download failed:" << message;
    setDownloadStage(UpdateDownloadStage::Failed);
}

void UpdateChecker::cancelDownload()
{
    if (!m_downloadReply) return;
    m_downloadCancelled = true;
    m_downloadError.clear();
    m_downloadReply->abort();   // synthesizes finished(); one teardown path
}

QString UpdateChecker::downloadedFileDir() const
{
    if (m_downloadedFilePath.isEmpty()) return QString();
    return QFileInfo(m_downloadedFilePath).absolutePath();
}

void UpdateChecker::revealDownload()
{
    const QString dir = downloadedFileDir();
    if (dir.isEmpty()) return;
    // The directory, not the file: openUrl on a .dmg/.AppImage would launch it.
    openExternally(QUrl::fromLocalFile(dir).toString());
}

void UpdateChecker::openReleasePage()
{
    openExternally(m_releaseUrl.isEmpty() ? QString::fromLatin1(kFallbackReleasesUrl)
                                          : m_releaseUrl);
}

void UpdateChecker::openExternally(const QString& url)
{
    if (url.isEmpty()) return;

#ifdef Q_OS_LINUX
    // In an AppImage, AppRun exports LD_LIBRARY_PATH pointing at the bundle's
    // own Qt/glibc. xdg-open inherits it and then tries to run *host* binaries
    // against those libraries — the classic "clicking a link does nothing"
    // AppImage failure. Hand it a scrubbed environment instead.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("LD_LIBRARY_PATH"));
    QProcess p;
    p.setProgram(QStringLiteral("xdg-open"));
    p.setArguments({url});
    p.setProcessEnvironment(env);
    if (p.startDetached()) return;
    qWarning() << "UpdateChecker: xdg-open failed, falling back to QDesktopServices";
#endif

    QDesktopServices::openUrl(QUrl(url));
}
