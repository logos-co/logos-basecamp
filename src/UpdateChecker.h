#pragma once

#include "UpdateEnums.h"
#include "UpdateInfo.h"

#include <QObject>
#include <QPointer>
#include <QString>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

// UpdateChecker — the host app's own staleness signal (issue #319).
//
// Basecamp's package manager already tells you when a *module* is out of date
// (AppsModel::recomputeInstallStatus); nothing told you when Basecamp itself was.
// A user on 0.2.1 with 0.2.3 out shipped a stale bundled delivery_module, which
// presented as a runtime bug rather than "you're three versions behind".
//
// Two jobs, in order:
//   1. Compare the baked-in VERSION against GitHub's latest release, and say so.
//   2. Optionally download the matching platform asset and tell the user what to
//      do with it. It does NOT self-install — no relaunch, no execve, no writing
//      into the app bundle. That's tier (c) and wants code signing first.
//
// Constructed as a fourth child of MainUIBackend, deliberately outside the
// CoreModuleManager/UIPluginManager/PackageCoordinator triangle: it touches no
// logos_core_* C API and no package_manager IPC. All state reaches QML via
// one-line delegations on MainUIBackend.
//
// NOTE this is the first outbound network request the host process makes. The
// deny-all QNetworkAccessManager in src/restricted/ applies only to sandboxed
// *plugin* QML engines (QmlSandbox::configure) and does not constrain this class.
class UpdateChecker : public QObject {
    Q_OBJECT

public:
    explicit UpdateChecker(QObject* parent = nullptr);
    ~UpdateChecker() override;

    UpdateCheckState::Value    checkState() const    { return m_checkState; }
    UpdateDownloadStage::Value downloadStage() const { return m_downloadStage; }

    bool    updateAvailable() const { return m_checkState == UpdateCheckState::UpdateAvailable; }
    QString currentVersion() const  { return m_currentVersion; }
    QString latestVersion() const   { return m_latestVersion; }
    QString releaseUrl() const      { return m_releaseUrl; }
    bool    downloadSupported() const { return !m_assetUrl.isEmpty(); }

    qreal   downloadProgress() const  { return m_downloadProgress; }
    QString progressText() const      { return m_progressText; }
    QString downloadedFilePath() const { return m_downloadedFilePath; }
    QString downloadedFileDir() const;
    QString downloadError() const     { return m_downloadError; }
    QString installHint() const       { return m_installHint; }

public slots:
    // Idempotent while a check is in flight. Safe to call from QML repeatedly.
    void checkForUpdates();
    void startDownload();
    void cancelDownload();
    // Opens the *containing directory*, not the file — openUrl on the file would
    // launch the DMG/AppImage instead of revealing it.
    void revealDownload();
    void openReleasePage();

signals:
    void checkStateChanged();
    void downloadStateChanged();

private:
    void setCheckState(UpdateCheckState::Value s);
    void setDownloadStage(UpdateDownloadStage::Value s);
    void failDownload(const QString& message);
    void onCheckFinished();
    void onDownloadReadyRead();
    void onDownloadFinished();
    void finalizeDownload();
    void teardownDownload();
    // Cancels in-flight work and removes the partial file. Called from both the
    // destructor and QCoreApplication::aboutToQuit, because plugin teardown is
    // not guaranteed to run before process exit.
    void abortAll();
    void openExternally(const QString& url);

    // Destroying a QNetworkAccessManager that still owns live replies is
    // unsupported by Qt and crashes intermittently inside QNetworkReplyHttpImpl
    // — and shutdown-tests.mjs quits the app after only 2s, so this path is
    // exercised on every run.
    //
    // The guarantee comes from abortAll() (called from both ~UpdateChecker and
    // aboutToQuit), NOT from member declaration order: QPointer is non-owning
    // and m_nam is a raw pointer, so neither destructor frees anything here —
    // the QNAM is freed later, by QObject child destruction.
    QNetworkAccessManager* m_nam = nullptr;

    QPointer<QNetworkReply> m_checkReply;
    QPointer<QNetworkReply> m_downloadReply;
    QFile*  m_downloadFile = nullptr;
    bool    m_downloadCancelled = false;
    qint64  m_bytesWritten = 0;

    UpdateCheckState::Value    m_checkState    = UpdateCheckState::Idle;
    UpdateDownloadStage::Value m_downloadStage = UpdateDownloadStage::Idle;

    QString m_currentVersion;
    QString m_latestVersion;
    QString m_releaseUrl;

    // Resolved once from the release payload; empty when no asset fits this
    // platform, which is what downloadSupported() reports.
    QString m_assetUrl;
    QString m_assetName;
    qint64  m_assetSize = 0;

    qreal   m_downloadProgress = -1.0;   // -1 = indeterminate
    QString m_progressText;
    QString m_downloadedFilePath;
    QString m_partFilePath;
    QString m_downloadError;
    QString m_installHint;
};
