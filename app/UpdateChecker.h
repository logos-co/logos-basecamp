#pragma once

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSaveFile>
#include <QString>

class QNetworkReply;

// UpdateChecker — the Dashboard's "Check for updates" button.
//
// Release builds check() once at launch, which feeds the sidebar's "New version
// available" label; dev builds only check when the button is pressed. check()
// reads the latest stable GitHub release, update() downloads the asset for this OS/CPU
// and applies it the way that platform installs Basecamp:
//   * Linux AppImage — replaces the running $APPIMAGE in place; restart() relaunches it.
//   * Windows        — restart() runs the downloaded setup.exe and quits.
//   * macOS          — opens the downloaded .dmg for the usual drag to Applications.
//   * anything else  — leaves the file in the Downloads folder and shows it.
class UpdateChecker : public QObject {
    Q_OBJECT
    // idle | checking | upToDate | available | downloading | ready | error
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool canRestart READ canRestart NOTIFY changed)

public:
    explicit UpdateChecker(const QString& currentVersion, QObject* parent = nullptr);

    QString state() const { return m_state; }
    QString latestVersion() const { return m_latestVersion; }
    double progress() const { return m_progress; }
    QString message() const { return m_message; }
    bool canRestart() const;

    Q_INVOKABLE void check();
    Q_INVOKABLE void update();
    Q_INVOKABLE void restart();

signals:
    void changed();

private:
    void setState(const QString& state, const QString& message = {});
    void finishDownload();

    QString m_currentVersion;
    QString m_state = QStringLiteral("idle");
    QString m_latestVersion;
    QString m_message;
    double m_progress = 0;
    QString m_assetUrl;
    QString m_assetName;
    qint64 m_assetSize = 0;
    QString m_assetDigest;  // "sha256:<hex>", as GitHub publishes it
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_targetPath;   // where the finished download ends up
    QSaveFile m_file;       // swapped into m_targetPath only on commit()
    QNetworkAccessManager m_nam;
    QPointer<QNetworkReply> m_reply;
};
