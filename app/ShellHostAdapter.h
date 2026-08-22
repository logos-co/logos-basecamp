#pragma once

#include "IShellHost.h"

#include <QObject>
#include <QPointer>
#include <QString>

class MainUIBackend;

// ─────────────────────────────────────────────────────────────────────────────
// ShellHostAdapter — the host's side of the shell boundary. Implements
// IShellHost over MainUIBackend and translates its five signals into
// IShellObserver calls; every operation is a one-line delegation.
//
// It exists so the shell never names MainUIBackend, whose header pulls in
// InstallEnums, the models and logos_api.h and whose signals carry
// InstallStage::Value — none of which a Qt-only shell could compile against.
//
// Owned by Window, alongside the MainUIBackend it wraps.
// ─────────────────────────────────────────────────────────────────────────────
class ShellHostAdapter : public QObject, public IShellHost {
    Q_OBJECT

public:
    // `backend` is borrowed; Window owns both and destroys this adapter first.
    explicit ShellHostAdapter(MainUIBackend* backend, QObject* parent = nullptr);
    ~ShellHostAdapter() override;

    // ── IShellHost ──────────────────────────────────────────────────────────
    QObject* backendObject() override;
    int      currentSectionIndex() const override;
    void     setCurrentSectionIndex(int index) override;
    void     loadUiModule(const QString& name) override;
    void     unloadUiModule(const QString& name) override;
    void     setCurrentVisibleApp(const QString& name) override;
    QString  displayNameFor(const QString& name) const override;
    void     setObserver(IShellObserver* observer) override;

private:
    // QPointer: Window deletes the backend during teardown, and a queued
    // backend signal can still be in flight when it does.
    QPointer<MainUIBackend> m_backend;

    // Raw: IShellObserver is not a QObject, so QPointer cannot track it.
    // destroyShell() nulls it; forwards are guarded anyway, since
    // QTimer::singleShot(0, ...) and a 30s ViewModuleHost timeout can both
    // fire after teardown has begun.
    IShellObserver* m_observer = nullptr;
};
