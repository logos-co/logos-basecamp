#pragma once

#include <QString>

class QObject;
class QWidget;

// ─────────────────────────────────────────────────────────────────────────────
// IShellHost / IShellObserver — the whole surface between the Basecamp host and
// its UI shell.
//
// The shell is handed one IShellHost* and can express nothing beyond the eight
// operations below. That is the point: it holds no LogosAPI*, no QtLogosCore*,
// and no TokenManager access, so it cannot mint identities, read the token
// store, or drive the core lifecycle even by accident. The host keeps those and
// exposes only what a shell legitimately needs.
//
// ── Why these types and no others ───────────────────────────────────────────
// Only QObject*, QWidget* and Qt value types cross. Once the shell becomes a
// separate image again, that is what lets it link Qt and nothing else — no
// logos runtime, and therefore no second copy of any runtime singleton. The
// symbol gate enforces exactly that, so widening this surface with a logos type
// is not a style question: it will fail the build.
//
// backendObject() is the deliberate escape hatch, and it is safe for the same
// reason: QML resolves everything on it dynamically through the metaobject, so
// no C++ type from the host's side has to be nameable by the shell.
//
// ── Versioning ──────────────────────────────────────────────────────────────
// IShellHost_abi below is compiled into BOTH sides. Bump it on ANY change to
// either vtable — added, removed, reordered or re-signatured methods. Once the
// shell is a plugin, a stale one is a silent vtable mismatch rather than a link
// error, and the version check is the only thing standing between that and a
// jump through a garbage slot.
//
// There is deliberately no source-drift guard script here (contrast
// logos-module-builder/tests/view-interface-abi.py, which exists because
// LogosViewReplicaFactory has two independently maintained copies in two repos
// with no dependency edge). This header is carried by the `component-interfaces`
// INTERFACE library and included by both targets, so there is exactly one copy
// and source drift is impossible by construction. Binary drift is the real
// risk, and that is what IShellHost_abi covers.
// ─────────────────────────────────────────────────────────────────────────────

// Bump on ANY vtable change to IShellHost or IShellObserver.
constexpr int IShellHost_abi = 1;

// Host → shell notifications. Implemented shell-side by MainContainer.
//
// Plain virtuals rather than signals on purpose: a signal/slot connection would
// make the two sides agree on a metaobject, which is exactly the coupling the
// boundary exists to avoid. These are called synchronously on the GUI thread.
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

    // The object QML binds as the `backend` context property. Returned as a
    // bare QObject* so the shell never names a host C++ type; QML resolves
    // properties, signals and slots on it through the metaobject.
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
    // can land after teardown has begun. QPointer cannot help here —
    // IShellObserver is not a QObject.
    virtual void setObserver(IShellObserver* observer) = 0;
};
