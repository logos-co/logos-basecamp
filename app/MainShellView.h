#pragma once

#include "IShellView.h"

#include <QObject>
#include <QPointer>

class MainContainer;

// ─────────────────────────────────────────────────────────────────────────────
// MainShellView — the IShellView implementation for Basecamp's own UI shell.
//
// A QObject with Q_INTERFACES so that `qobject_cast<IShellView*>` works on it.
// That cast is what Window will use once this class ships inside the main_ui
// plugin again; today Window constructs it directly, and the cast path costs
// nothing to keep working in the meantime.
//
// Deliberately holds no state beyond the shell it built: everything the shell
// needs arrives through the IShellHost* it is handed.
// ─────────────────────────────────────────────────────────────────────────────
class MainShellView : public QObject, public IShellView {
    Q_OBJECT
    Q_INTERFACES(IShellView)

public:
    explicit MainShellView(QObject* parent = nullptr);
    ~MainShellView() override;

    // ── IShellView ──────────────────────────────────────────────────────────
    QWidget* createShell(IShellHost* host) override;
    void     destroyShell(QWidget* widget) override;
    int      hostAbiVersion() const override;

private:
    // QPointer auto-nulls when the widget is destroyed by its Qt parent (Window
    // owns it as the central widget), so a later destroyShell() is a no-op
    // rather than a double delete. The pre-fold MainUIPlugin carried exactly
    // this guard and it is the reason it survived process exit.
    QPointer<MainContainer> m_shell;
};
