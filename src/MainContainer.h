#pragma once

#include <QWidget>
#include <QHBoxLayout>
#include <QStackedWidget>

class QQuickWidget;
class MainUIBackend;
class MdiView;
class LogosAPI;

class MainContainer : public QWidget
{
    Q_OBJECT

public:
    explicit MainContainer(LogosAPI* logosAPI = nullptr, QWidget* parent = nullptr);
    ~MainContainer();

    // Get the MDI view
    MdiView* getMdiView() const { return m_mdiView; }
    
    // Get the backend
    MainUIBackend* getBackend() const { return m_backend; }
    
    // Get the LogosAPI instance
    LogosAPI* getLogosAPI() const { return m_logosAPI; }

protected:
    // Kept in sync with the full MainContainer geometry — the overlay
    // widget floats over both the sidebar and the content stack, so it
    // can't sit in the HBoxLayout.
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onViewIndexChanged();
    void onNavigateToApps();
    void onPluginWindowRequested(QWidget* widget, const QString& title);
    void onPluginWindowRemoveRequested(QWidget* widget);
    void onPluginWindowActivateRequested(QWidget* widget);
    // Called from QML whenever the combined visibility of the three
    // overlay dialogs flips. We use it to toggle mouse-event
    // passthrough on the overlay QQuickWidget — transparent to mouse
    // input when no dialog is open (so the sidebar / content still
    // receive clicks), intercepting when one is.
    void onOverlayActiveChanged(bool active);

private:
    void setupUi();

    // Main layout
    QHBoxLayout* m_mainLayout;

    // Sidebar (QML)
    QQuickWidget* m_sidebarWidget;

    // Content area
    QStackedWidget* m_contentStack;

    // MdiView (C++ widget for Apps)
    MdiView* m_mdiView;

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

    // Backend
    MainUIBackend* m_backend;

    // LogosAPI instance
    LogosAPI* m_logosAPI;
};

