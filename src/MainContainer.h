#pragma once

#include "IShellHost.h"

#include <QWidget>
#include <QHBoxLayout>
#include <QStackedWidget>

class QQuickWidget;
class WorkspaceArea;
class ShortcutBridge;

// MainContainer — the UI shell.
//
// Holds exactly one host-side pointer: an IShellHost*. It has no LogosAPI*, no
// QtLogosCore*, and no MainUIBackend*; every host operation it needs goes
// through the eight methods on that interface, and QML reaches the backend as
// an opaque QObject* via IShellHost::backendObject(). That is what lets this
// class — and everything below it — compile against Qt alone.
class MainContainer : public QWidget, public IShellObserver
{
    Q_OBJECT

public:
    // `host` is borrowed and outlives this widget. Must not be null.
    explicit MainContainer(IShellHost* host, QWidget* parent = nullptr);
    ~MainContainer();

    // Get the workspace area
    WorkspaceArea* getWorkspaceArea() const { return m_workspaceArea; }

    // Stops the host from calling back into this object. Idempotent, and safe
    // to call more than once — MainShellView::destroyShell() calls it before
    // deleting, and the destructor calls it again for the path where Qt's
    // parent-child teardown gets here first.
    void detachFromHost();

    // ── IShellObserver ──────────────────────────────────────────────────────
    void onSectionIndexChanged(int index) override;
    void onNavigateToApps() override;
    void onPluginWindowRequested(QWidget* widget, const QString& title) override;
    void onPluginWindowRemoveRequested(QWidget* widget) override;
    void onPluginWindowActivateRequested(QWidget* widget) override;

protected:
    // Kept in sync with the full MainContainer geometry — the overlay
    // widget floats over both the sidebar and the content stack, so it
    // can't sit in the HBoxLayout.
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // Called from QML whenever the combined visibility of the three
    // overlay dialogs flips. We use it to toggle mouse-event
    // passthrough on the overlay QQuickWidget — transparent to mouse
    // input when no dialog is open (so the sidebar / content still
    // receive clicks), intercepting when one is.
    void onOverlayActiveChanged(bool active);
    void onSidebarTooltipRequested(const QString& text, qreal y);

private:
    void setupUi();

    // Main layout
    QHBoxLayout* m_mainLayout;

    // Sidebar (QML)
    QQuickWidget* m_sidebarWidget;

    // Content area
    QStackedWidget* m_contentStack;

    // Workspace (QDockWidget-based, replaces the old QMdiArea workspace)
    WorkspaceArea* m_workspaceArea;

    QWidget* m_pmuiWidget = nullptr;
    bool m_suppressNextNavToApps = false;

    // Content views (QML for Dashboard, Modules, PackageManager, Settings)
    QQuickWidget* m_contentWidget;

    // Full-window overlay for the dependency-aware dialogs
    // (missing-deps popup, cascade-unload/uninstall confirmations).
    // Lives outside the content stack so it's visible regardless of
    // which stack page is current — previously the dialogs were
    // anchored inside m_contentWidget and never rendered while the
    // Apps/MDI screen was active.
    QQuickWidget* m_overlayWidget;

    // The host. Not owned — Window owns it and it outlives this widget.
    IShellHost* m_host;

    ShortcutBridge* m_shortcutBridge = nullptr;
};

