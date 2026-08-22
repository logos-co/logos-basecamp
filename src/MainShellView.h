#pragma once

#include "IShellView.h"

#include <QObject>
#include <QPointer>

class MainContainer;

// ─────────────────────────────────────────────────────────────────────────────
// MainShellView — the IShellView implementation for Basecamp's own UI shell.
//
// A QObject with Q_INTERFACES + Q_PLUGIN_METADATA: this is the class
// QPluginLoader instantiates, and `qobject_cast<IShellView*>` on the resulting
// instance is how Window reaches it. A real cast, not
// QMetaObject::invokeMethod("createWidget") — the pre-fold code used the string
// form, where changing an argument type still compiles on both sides and simply
// misses at runtime.
//
// Deliberately holds no state beyond the shell it built: everything the shell
// needs arrives through the IShellHost* it is handed.
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
    // rather than a double delete. The pre-fold MainUIPlugin carried exactly
    // this guard and it is the reason it survived process exit.
    QPointer<MainContainer> m_shell;
};
