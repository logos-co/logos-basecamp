#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class QTimer;

// One approved package change at a time, whichever app asks: a
// basecamp.packages.confirm_* waits while another's dialog is on screen, and
// once approved, while that change is still being made. The change is made
// once package_manager shows it on disk, the app that made it says how it
// ended, asks again (an installer app asks only after its last change ended),
// closes or stops, or a while has passed with nothing of it moving.
class ConfirmationQueue : public QObject {
    Q_OBJECT
public:
    struct Request {
        QString id;
        QString intent;
        QVariantMap params;
        QString requester;
    };
    // Shows it, and answers it later; false when it couldn't be shown and was
    // answered already.
    using Begin = std::function<bool(const Request&)>;

    explicit ConfirmationQueue(QObject* parent = nullptr);

    void setBegin(Begin begin) { m_begin = std::move(begin); }
    // How long an approved change may go with nothing of it moving before the
    // next is shown anyway.
    void setChangeTimeout(int ms) { m_changeTimeoutMs = ms; }

    // Shown now, or once the ones before it are through.
    void submit(const Request& request);
    // Its answer: approved with `data` ({plan} or {file}), or not.
    void answered(const QString& id, bool approved, const QVariant& data);
    // Its request ended unanswered: dropped if waiting, ended if on screen.
    void withdrawn(const QString& id);
    // package_manager's installed packages, after a change.
    void installedChanged(const QVariantList& installed);
    // A package of the change moving (its download, or its files installed or
    // removed): the change waits on, however long that takes.
    void progressed(const QString& name);
    // Something installed or removed, whichever package: the change waits on.
    void activity();
    // The app that asked is done with its change: it said how the change
    // ended, or it closed or stopped.
    void changeEnded(const QString& requester);

    QString onScreen() const { return m_onScreen.id; }
    int waiting() const { return int(m_waiting.size()); }
    // The requester whose approved change is being made, or "".
    QString changing() const { return m_change.requester; }

private:
    // As approved; "" is any.
    struct Release {
        QString version;
        QString rootHash;
    };
    struct Change {
        QString requester;
        QHash<QString, Release> installs;
        QStringList removes;
    };

    void next();
    void endChange();
    bool madeOn(const QVariantList& installed) const;

    Begin m_begin;
    // Past the downloader's own 60 s stall timeout: a stalled download has
    // failed, and stopped moving, by then.
    int m_changeTimeoutMs = 2 * 60 * 1000;
    Request m_onScreen;
    QList<Request> m_waiting;
    Change m_change;
    QTimer* m_changeTimer;
    bool m_beginning = false;
};
