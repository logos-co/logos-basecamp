#pragma once

#include <QtPlugin>

class QWidget;
class IShellHost;

// ─────────────────────────────────────────────────────────────────────────────
// IShellView — the UI shell, from the host's side.
//
// This is the seam that replaces IComponent for the main UI. IComponent stays
// exactly as it is, for the third-party legacy widget plugins PluginLoader
// still loads; it is not reused here because its signature
// (`createWidget(LogosAPI*)`) names the very type this boundary exists to keep
// out of the shell.
//
// Today the implementation is compiled into the executable and Window
// constructs it directly. Once the shell is a plugin again, Window will resolve
// it with a real qobject_cast<IShellView*> on the QPluginLoader instance —
// NOT a QMetaObject::invokeMethod("createWidget") string call, which is what
// the pre-fold code did and is a silent-failure path: change an argument type
// and both sides still compile, then miss at runtime.
// ─────────────────────────────────────────────────────────────────────────────
class IShellView {
public:
    virtual ~IShellView() = default;

    // Builds the shell widget. `host` is borrowed and outlives the shell; a
    // null host is FATAL, not a degraded mode — there is no meaningful shell
    // without one, and failing here beats a null dereference later.
    virtual QWidget* createShell(IShellHost* host) = 0;

    // Tears the shell down. Implementations must detach from the host
    // (setObserver(nullptr)) before returning, so no late callback can reach a
    // destroyed observer.
    virtual void destroyShell(QWidget* widget) = 0;

    // Must return IShellHost_abi as compiled into the shell. The host compares
    // it against its own and refuses to continue on a mismatch.
    virtual int hostAbiVersion() const = 0;
};

#define IShellView_iid "com.logos.basecamp.IShellView/1.0"
Q_DECLARE_INTERFACE(IShellView, IShellView_iid)
