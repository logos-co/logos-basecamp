#include "UpdateChecker.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

namespace {

const char* kLatestReleaseUrl = "https://api.github.com/repos/logos-co/logos-basecamp/releases/latest";

// Release assets are named LogosBasecamp-Desktop-v<ver>-<sha>-<arch><suffix>.
QString platformSuffix()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("-setup.exe");
#elif defined(Q_OS_MACOS)
    return QStringLiteral(".dmg");
#else
    return QStringLiteral(".AppImage");
#endif
}

QString platformArch()
{
    // Qt says arm64; the release assets say aarch64.
    const QString arch = QSysInfo::currentCpuArchitecture();
    return arch == QLatin1String("arm64") ? QStringLiteral("aarch64") : arch;
}

// Only a plain "x.y.z" counts. Dev builds ("pre-release-<sha>" or empty) are
// never "up to date", so the button still offers the latest release.
QVersionNumber strictVersion(QString v)
{
    if (v.startsWith(QLatin1Char('v')))
        v.remove(0, 1);
    qsizetype suffix = 0;
    const QVersionNumber n = QVersionNumber::fromString(v, &suffix);
    return suffix == v.size() ? n : QVersionNumber();
}

QString runningAppImage()
{
#ifdef Q_OS_LINUX
    return qEnvironmentVariable("APPIMAGE");
#else
    return {};
#endif
}

} // namespace

UpdateChecker::UpdateChecker(const QString& currentVersion, QObject* parent)
    : QObject(parent)
    , m_currentVersion(currentVersion)
{
    // Check once at launch so the sidebar can say a new version is available.
    // Release builds only: a dev build would always be "behind" the release.
    if (!strictVersion(currentVersion).isNull())
        QTimer::singleShot(0, this, &UpdateChecker::check);
}

bool UpdateChecker::canRestart() const
{
#ifdef Q_OS_WIN
    return m_state == QLatin1String("ready");
#else
    return m_state == QLatin1String("ready") && m_targetPath == runningAppImage();
#endif
}

void UpdateChecker::setState(const QString& state, const QString& message)
{
    m_state = state;
    m_message = message;
    emit changed();
}

void UpdateChecker::check()
{
    if (m_reply)
        return;
    setState(QStringLiteral("checking"));

    QNetworkRequest request{QUrl(QString::fromLatin1(kLatestReleaseUrl))};
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("LogosBasecamp"));
    m_reply = m_nam.get(request);
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply.data()]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setState(QStringLiteral("error"), tr("Couldn't check for updates: %1").arg(reply->errorString()));
            return;
        }
        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        m_latestVersion = release.value(QLatin1String("tag_name")).toString();
        if (m_latestVersion.startsWith(QLatin1Char('v')))
            m_latestVersion.remove(0, 1);

        const QVersionNumber current = strictVersion(m_currentVersion);
        const QVersionNumber latest = strictVersion(m_latestVersion);
        if (latest.isNull()) {
            setState(QStringLiteral("error"), tr("Couldn't read the latest release."));
            return;
        }
        if (!current.isNull() && latest <= current) {
            setState(QStringLiteral("upToDate"));
            return;
        }

        m_assetUrl.clear();
        const QString suffix = platformSuffix();
        const QString arch = QLatin1Char('-') + platformArch();
        for (const QJsonValue& v : release.value(QLatin1String("assets")).toArray()) {
            const QJsonObject asset = v.toObject();
            const QString name = asset.value(QLatin1String("name")).toString();
            // A bare file name: it becomes part of a path below.
            if (name.endsWith(suffix) && name.contains(arch) && QFileInfo(name).fileName() == name) {
                m_assetName = name;
                m_assetUrl = asset.value(QLatin1String("browser_download_url")).toString();
                m_assetSize = asset.value(QLatin1String("size")).toInteger();
                m_assetDigest = asset.value(QLatin1String("digest")).toString();
                break;
            }
        }
        if (m_assetUrl.isEmpty()) {
            setState(QStringLiteral("error"),
                     tr("Version %1 has no download for this system (%2).").arg(m_latestVersion, platformArch()));
            return;
        }
        setState(QStringLiteral("available"));
    });
}

void UpdateChecker::update()
{
    if (m_reply || m_state != QLatin1String("available"))
        return;

    // A running AppImage is replaced in place when its folder is writable;
    // otherwise, and for every other install, the file lands in Downloads.
    const QString appImage = runningAppImage();
    m_file.setFileName(appImage);
    if (appImage.isEmpty() || !m_file.open(QIODevice::WriteOnly)) {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (dir.isEmpty() || !QDir().mkpath(dir))
            dir = QDir::tempPath();
        m_file.setFileName(QDir(dir).filePath(m_assetName));
        if (!m_file.open(QIODevice::WriteOnly)) {
            setState(QStringLiteral("error"), tr("Can't write %1: %2").arg(m_file.fileName(), m_file.errorString()));
            return;
        }
    }
    m_targetPath = m_file.fileName();

    m_hash.reset();
    m_progress = 0;
    setState(QStringLiteral("downloading"));
    QNetworkRequest request{QUrl(m_assetUrl)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("LogosBasecamp"));
    m_reply = m_nam.get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        const QByteArray chunk = m_reply->readAll();
        m_hash.addData(chunk);
        m_file.write(chunk);
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total <= 0)
            total = m_assetSize;
        if (total > 0) {
            m_progress = double(received) / double(total);
            emit changed();
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, &UpdateChecker::finishDownload);
}

void UpdateChecker::finishDownload()
{
    QNetworkReply* reply = m_reply;
    reply->deleteLater();
    const QByteArray rest = reply->readAll();
    m_hash.addData(rest);
    m_file.write(rest);

    QString failure;
    if (reply->error() != QNetworkReply::NoError)
        failure = reply->errorString();
    else if (m_assetSize > 0 && m_file.size() != m_assetSize)
        failure = tr("got %1 of %2 bytes").arg(m_file.size()).arg(m_assetSize);
    else if (m_assetDigest.startsWith(QLatin1String("sha256:"))
             && m_assetDigest.mid(7) != QString::fromLatin1(m_hash.result().toHex()))
        failure = tr("checksum mismatch");
    if (!failure.isEmpty())
        m_file.cancelWriting();   // commit() then just discards the temporary file
    if (!m_file.commit() && failure.isEmpty())
        failure = tr("couldn't save %1: %2").arg(m_targetPath, m_file.errorString());
    if (!failure.isEmpty()) {
        setState(QStringLiteral("available"), tr("Download failed: %1").arg(failure));
        return;
    }
    if (m_targetPath.endsWith(QLatin1String(".AppImage")))
        QFile::setPermissions(m_targetPath, QFile::permissions(m_targetPath)
                              | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);

    m_progress = 1;
#if defined(Q_OS_WIN)
    setState(QStringLiteral("ready"), tr("Version %1 downloaded. Restart Basecamp to install it.").arg(m_latestVersion));
#elif defined(Q_OS_MACOS)
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_targetPath));
    setState(QStringLiteral("ready"),
             tr("Opened %1. Quit Basecamp, then drag it into Applications.").arg(m_assetName));
#else
    if (m_targetPath == runningAppImage()) {
        setState(QStringLiteral("ready"), tr("Version %1 is installed. Restart Basecamp to use it.").arg(m_latestVersion));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_targetPath).absolutePath()));
    setState(QStringLiteral("ready"), tr("Saved to %1").arg(m_targetPath));
#endif
}

void UpdateChecker::restart()
{
    if (!canRestart())
        return;
    QProcess p;
    // stdout/stderr are LogSink's capture pipe here; a child holding them
    // keeps LogSink::stop() from ever seeing EOF, so this process never exits.
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    // The installer replaces this install; it must not be running meanwhile.
    p.setProgram(m_targetPath);
#else
    // Start the new AppImage only once this process has gone, or it would hand
    // itself to this instance through SingleInstanceGuard and exit. Its
    // environment is the AppImage runtime's, pointing into our mount; drop it.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* var : {"LD_LIBRARY_PATH", "APPDIR", "APPIMAGE", "ARGV0", "OWD", "XKB_CONFIG_ROOT", "QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH"})
        env.remove(QString::fromLatin1(var));
    p.setProcessEnvironment(env);
    p.setProgram(QStringLiteral("/bin/sh"));
    p.setArguments({QStringLiteral("-c"), QStringLiteral("while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; exec \"$0\""),
                    m_targetPath, QString::number(QCoreApplication::applicationPid())});
#endif
    p.startDetached();
    QCoreApplication::quit();
}
