#pragma once

#include "IShellHost.h"

#include <QWidget>
#include <QHBoxLayout>
#include <QList>
#include <QPointer>
#include <QStackedWidget>

class QQuickWidget;
class QTimer;
namespace Logos { class ThemeManager; }
class WorkspaceArea;
class ShortcutBridge;

// MainContainer — the UI shell. Holds exactly one host-side pointer, an
// IShellHost*: no LogosAPI*, no QtLogosCore*, no MainUIBackend*. QML reaches
// the backend as an opaque QObject* via IShellHost::backendObject(). That is
// what lets this class, and everything below it, compile against Qt alone.
class MainContainer : public QWidget, public IShellObserver
{
    Q_OBJECT

public:
    // `host` is borrowed and outlives this widget. Must not be null.
    explicit MainContainer(IShellHost* host, QWidget* parent = nullptr);
    ~MainContainer();

    WorkspaceArea* getWorkspaceArea() const { return m_workspaceArea; }

    // Stops the host from calling back into this object. Idempotent:
    // MainShellView::destroyShell() calls it before deleting, and the
    // destructor calls it again for the path where Qt's parent-child teardown
    // gets here first.
    void detachFromHost();

    // ── IShellObserver ──────────────────────────────────────────────────────
    void onSectionIndexChanged(int index) override;
    void onNavigateToApps() override;
    void onPluginWindowRequested(QWidget* widget, const QString& title) override;
    void onPluginWindowRemoveRequested(QWidget* widget) override;
    // Presentation seam. Branches on the widget pointer because this class
    // decided where each widget was mounted.
    void onPresentAppRequested(QWidget* widget) override;
    void onAppStopped(QWidget* widget, const QString& name) override;

protected:
    // Keeps the overlay sized to the full MainContainer: it floats over both
    // the sidebar and the content stack, so it can't sit in the HBoxLayout.
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // Called from QML when the combined visibility of the three overlay
    // dialogs flips. Toggles mouse-event passthrough on the overlay
    // QQuickWidget: transparent when no dialog is open, so the sidebar and
    // content still receive clicks, intercepting when one is.
    void onOverlayActiveChanged(bool active);
    void onSidebarTooltipRequested(const QString& text, qreal y);
    void onOnboardingFinished();
    void onOnlineChanged();
    void closeOfflineToast();
    void updateOfflineToast();
    void maskOfflineToast();
    void onStoppedAppReopenRequested(const QString& name);
    void onStoppedAppCloseRequested(const QString& name);

private:
    void wireSidebar(QObject* sidebarRoot);
    QQuickWidget* makeAppStoppedPanel(const QString& name);
    bool offersSection(int section) const;
    void invokeOpenApp(const QString& name, const QString& repositoryUrl);
    void setupUi();

    QHBoxLayout* m_mainLayout;

    QQuickWidget* m_sidebarWidget;

    QStackedWidget* m_contentStack;

    // Workspace (QDockWidget-based, replaces the old QMdiArea workspace)
    WorkspaceArea* m_workspaceArea;

    // A default app shown as a section (DefaultApps.h): its page of the content
    // stack, the app's widget once it arrives.
    struct SectionPage {
        int section;
        int stackIndex;
        QString app;
        // QPointer, not a raw pointer: this widget is owned by the HOST side,
        // which deleteLater()s it on unload (UIPluginManager::unloadUiModuleImpl
        // and ::teardownUiPluginWidget both do). As a raw pointer it was read
        // after the widget had been freed and never became null again, so the
        // section could never reload. Same guard the host applies to its own
        // widget maps.
        QPointer<QWidget> widget;
        // Shown in the page's place while the app's backend is stopped.
        QPointer<QWidget> stoppedPanel;
    };
    SectionPage* pageForSection(int section);
    SectionPage* pageForApp(const QString& app);
    SectionPage* pageForWidget(const QWidget* widget);
    QList<SectionPage> m_sectionPages;
    bool m_suppressNextNavToApps = false;

    // Content views (QML for Settings)
    QQuickWidget* m_contentWidget;

    // Full-window overlay for the dependency-aware dialogs
    // (missing-deps popup, cascade-unload/uninstall confirmations).
    // Lives outside the content stack so it's visible regardless of
    // which stack page is current — previously the dialogs were
    // anchored inside m_contentWidget and never rendered while the
    // Apps/MDI screen was active.
    QQuickWidget* m_overlayWidget;

    // First-run onboarding, over the whole window until it finishes; null
    // otherwise. Lives outside the layout, like the overlay.
    QPointer<QQuickWidget> m_onboardingWidget;

    bool isOffline() const;
    void placeOfflineToast();

    // Floats over the bottom of the content while offline, until closed;
    // comes back if still offline a while later, or offline again.
    QQuickWidget* m_offlineToast = nullptr;
    QTimer*       m_offlineToastReturn = nullptr;
    bool          m_offlineToastClosed = false;

    // Not owned — Window owns it and it outlives this widget.
    IShellHost* m_host;

    ShortcutBridge* m_shortcutBridge = nullptr;

    // Installs and switches themes. Given only to the content view, where
    // Settings → Appearance lives: apps may read the theme, not change it.
    Logos::ThemeManager* m_themeManager = nullptr;
};

