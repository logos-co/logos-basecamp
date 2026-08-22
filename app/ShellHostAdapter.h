#pragma once

#include "IShellHost.h"

#include <QObject>
#include <QPointer>
#include <QString>

class MainUIBackend;

// ─────────────────────────────────────────────────────────────────────────────
// ShellHostAdapter — the host's side of the shell boundary.
//
// Implements IShellHost over MainUIBackend and translates the five backend
// signals the shell cares about into IShellObserver virtual calls. It adds no
// policy of its own: every operation is a one-line delegation.
//
// It exists so the shell never names MainUIBackend. That matters twice over:
// MainUIBackend's header pulls in InstallEnums, the models and logos_api.h, and
// its signal signatures carry InstallStage::Value — none of which a Qt-only
// shell could compile against.
//
// Owned by Window, alongside the MainUIBackend it wraps. Neither is owned by
// the shell.
// ─────────────────────────────────────────────────────────────────────────────
class ShellHostAdapter : public QObject, public IShellHost {
    Q_OBJECT

public:
    // `backend` is borrowed and must outlive this adapter. Both are owned by
    // Window, which destroys the adapter first.
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
    // QPointer, not a raw pointer: Window deletes the backend during teardown,
    // and a queued backend signal can still be in flight when it does.
    QPointer<MainUIBackend> m_backend;

    // Raw by necessity — IShellObserver is not a QObject, so QPointer cannot
    // track it. The shell's destroyShell() contract is what nulls this; every
    // forward below is guarded anyway, because the host dispatches through
    // QTimer::singleShot(0, ...) and a 30s ViewModuleHost timeout, both of
    // which can fire after teardown has begun.
    IShellObserver* m_observer = nullptr;
};
