#pragma once

#include <QtPlugin>

class QWidget;
class IShellHost;

// ─────────────────────────────────────────────────────────────────────────────
// IShellView — the UI shell, from the host's side.
//
// The seam for the main UI. IComponent is not reused here: its signature
// (`createWidget(LogosAPI*)`) names the very type this boundary exists to keep
// out of the shell. It stays exactly as it is for the third-party legacy widget
// plugins PluginLoader still loads.
//
// Window resolves the shell with qobject_cast<IShellView*> on the QPluginLoader
// instance — NOT a QMetaObject::invokeMethod("createWidget") string call, which
// is a silent-failure path: change an argument type and both sides still
// compile, then miss at runtime.
// ─────────────────────────────────────────────────────────────────────────────
class IShellView {
public:
    virtual ~IShellView() = default;

    // Builds the shell widget. `host` is borrowed and outlives the shell; a
    // null host is FATAL, not a degraded mode — failing here beats a null
    // dereference later.
    virtual QWidget* createShell(IShellHost* host) = 0;

    // Tears the shell down. Must detach from the host (setObserver(nullptr))
    // before returning, so no late callback reaches a destroyed observer.
    virtual void destroyShell(QWidget* widget) = 0;

    // Must return IShellHost_abi as compiled into the shell. The host compares
    // it against its own and refuses to continue on a mismatch.
    virtual int hostAbiVersion() const = 0;
};

#define IShellView_iid "com.logos.basecamp.IShellView/1.0"
Q_DECLARE_INTERFACE(IShellView, IShellView_iid)
