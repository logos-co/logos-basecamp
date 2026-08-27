#pragma once

#include <QString>

class QObject;
class QWidget;

// ─────────────────────────────────────────────────────────────────────────────
// IShellHost / IShellObserver — the whole surface between the Basecamp host and
// its UI shell plugin.
//
// The shell is handed one IShellHost* and nothing else: no LogosAPI*, no
// QtLogosCore*, no TokenManager, so it cannot mint identities, read the token
// store or drive the core lifecycle even by accident.
//
// Only QObject*, QWidget* and Qt value types cross, which is what lets the
// shell link Qt and nothing else — no logos runtime, hence no second copy of
// any runtime singleton. The symbol gate enforces that: widening this surface
// with a logos type fails the build. backendObject() is the escape hatch and is
// safe for the same reason — QML resolves it through the metaobject, so the
// shell never has to name a host C++ type.
//
// IShellHost_abi is compiled into BOTH sides; bump it on ANY vtable change —
// added, removed, reordered or re-signatured methods. A stale plugin is a
// silent vtable mismatch rather than a link error, and the version check is the
// only thing standing between that and a jump through a garbage slot.
//
// The `component-interfaces` INTERFACE library carries this header to both
// targets, so there is exactly one copy and source drift is impossible by
// construction — no guard script needed here, unlike
// logos-module-builder/tests/view-interface-abi.py, which covers two
// independently maintained copies. Binary drift is the real risk, and that is
// what IShellHost_abi covers.
// ─────────────────────────────────────────────────────────────────────────────

// Bump on ANY vtable change to IShellHost or IShellObserver.
constexpr int IShellHost_abi = 1;

// Host → shell notifications. Implemented shell-side by MainContainer.
//
// Plain virtuals rather than signals: a signal/slot connection would make the
// two sides agree on a metaobject, exactly the coupling this boundary avoids.
// Called synchronously on the GUI thread.
class IShellObserver {
public:
    virtual ~IShellObserver() = default;

    virtual void onSectionIndexChanged(int index) = 0;
    virtual void onNavigateToApps() = 0;

    // Workspace/dock coordination. The QWidget* is owned by the host side and
    // reparented into the shell's workspace; the shell must not delete it.
    virtual void onPluginWindowRequested(QWidget* widget, const QString& title) = 0;
    virtual void onPluginWindowRemoveRequested(QWidget* widget) = 0;
    virtual void onPluginWindowActivateRequested(QWidget* widget) = 0;
};

// Shell → host operations. Implemented host-side by ShellHostAdapter.
class IShellHost {
public:
    virtual ~IShellHost() = default;

    // The object QML binds as the `backend` context property. Bare QObject* so
    // the shell never names a host C++ type; QML resolves properties, signals
    // and slots on it through the metaobject.
    virtual QObject* backendObject() = 0;

    virtual int  currentSectionIndex() const = 0;
    virtual void setCurrentSectionIndex(int index) = 0;

    virtual void loadUiModule(const QString& name) = 0;
    virtual void unloadUiModule(const QString& name) = 0;
    virtual void setCurrentVisibleApp(const QString& name) = 0;

    // Human-readable label for a module name, falling back to the name itself.
    virtual QString displayNameFor(const QString& name) const = 0;

    // nullptr detaches. The shell MUST call setObserver(nullptr) before it is
    // destroyed: host-side callbacks are dispatched from a
    // QTimer::singleShot(0, ...) and from a 30s ViewModuleHost timeout, so they
    // can land after teardown begins. QPointer cannot help — IShellObserver is
    // not a QObject.
    virtual void setObserver(IShellObserver* observer) = 0;
};
