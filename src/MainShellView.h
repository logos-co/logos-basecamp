#pragma once

#include "IShellView.h"

#include <QObject>
#include <QPointer>

class MainContainer;

// ─────────────────────────────────────────────────────────────────────────────
// MainShellView — the IShellView implementation for Basecamp's own UI shell.
//
// Q_INTERFACES + Q_PLUGIN_METADATA: QPluginLoader instantiates this class and
// Window reaches it with `qobject_cast<IShellView*>`. A real cast, not
// QMetaObject::invokeMethod by name — a signature change that way still
// compiles on both sides and misses only at runtime.
//
// Holds no state beyond the shell it built; the rest arrives via IShellHost*.
// ─────────────────────────────────────────────────────────────────────────────
class MainShellView : public QObject, public IShellView {
    Q_OBJECT
    Q_INTERFACES(IShellView)
    Q_PLUGIN_METADATA(IID IShellView_iid FILE "metadata.json")

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
    // rather than a double delete.
    QPointer<MainContainer> m_shell;
};
