#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

// One chooser on screen at a time, whichever broker asks for it: the dialog
// never changes under the person deciding on it. The next comes up only once
// the dialog has closed.
class IntentChooserQueue : public QObject {
    Q_OBJECT
public:
    struct Chooser {
        QString dispatchId;
        QString intent;
        QString requesterName;
        QVariantList providers;
    };

    using QObject::QObject;

    // Shown now, or once the ones before it have closed.
    void request(const Chooser& chooser);
    // Its request ended: dropped if waiting, closed if on screen.
    void withdraw(const QString& dispatchId);
    // The dialog has closed: the next one comes up.
    void closed(const QString& dispatchId);

    QString onScreen() const { return m_onScreen; }
    int waiting() const { return int(m_waiting.size()); }

signals:
    void show(const QString& dispatchId, const QString& intent, const QString& requesterName,
              const QVariantList& providers);
    void close(const QString& dispatchId);

private:
    void next();

    QString m_onScreen;
    QList<Chooser> m_waiting;
};
