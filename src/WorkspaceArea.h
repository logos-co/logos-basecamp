#pragma once

#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QStringList>

class QDockWidget;
class QTabBar;
class QHideEvent;
class QShowEvent;
class QQuickWidget;

// WorkspaceArea — Uses Qt's native tabifyDockWidget so dragging tabs
// to detach + dropping docks onto tab strips both work for free;
// reuses MdiView's tab styling to keep the look identical.
class WorkspaceArea : public QMainWindow
{
    Q_OBJECT

    // Read-only test hooks, read via inspector evaluate on objectName "workspace".
    Q_PROPERTY(int dockCount READ dockCount NOTIFY dockLayoutChanged)
    Q_PROPERTY(QStringList dockOrder READ dockOrder NOTIFY dockLayoutChanged)
    Q_PROPERTY(QString layoutMode READ layoutMode NOTIFY dockLayoutChanged)

public:
    explicit WorkspaceArea(QObject* backend = nullptr, QWidget* parent = nullptr);
    ~WorkspaceArea() override;

    int dockCount() const { return m_dockOrder.size(); }
    QStringList dockOrder() const { return m_dockOrder; }
    QString layoutMode() const {
        return m_sideBySide ? QStringLiteral("sideBySide")
                            : QStringLiteral("tabbed");
    }
    Q_INVOKABLE void closeDock(const QString& moduleName);

    void addPluginDock(QWidget* pluginWidget,
                       const QString& moduleName,
                       const QString& displayLabel = {});
    void removePluginDock(const QString& moduleName);
    void activatePluginDock(const QString& moduleName);
    void removePluginDock(QWidget* pluginWidget);
    void activatePluginDock(QWidget* pluginWidget);

    QDockWidget* dockFor(const QString& moduleName) const { return m_docks.value(moduleName); }
    QString nameForWidget(QWidget* w) const;
    QQuickWidget* welcomePageWidget() const { return m_welcomeWidget; }
    // Front-most plugin dock's QQuickWidget, or nullptr if none is up.
    // Used by ShortcutBridge to reach dock plugin QML shortcuts.
    QQuickWidget* activeDockWidget() const;

signals:
    void pluginClosed(const QString& moduleName);
    void dockLayoutChanged();
    void installClicked();
    // Emitted when the front-most dock changes (tab click, close, activate).
    // Empty string when no real dock is current (welcome page).
    void activeAppChanged(const QString& moduleName);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void customizeTabBarStyle(QTabBar* tabBar);
    void installTabBarCloseButtons(QTabBar* tabBar);
    void insetTabBarGeometry(QTabBar* tabBar, int insetPx);
    void styleAllTabBars();
    void updateQmlPluginActiveStates();
    void updateWelcomeVisibility();
    QString moduleNameForTabText(const QString& tabText) const;

    // Ensure the tab bar is visible even when only one real dock is open.
    void ensurePhantomTab();
    void removePhantom();

    void placeDockInGrid(QDockWidget* dock, int gridIndex);

    // DEV: Ctrl+Shift+L toggles between tabbed and side-by-side.
    void toggleLayoutModeForTesting();

    static constexpr int kColsPerRow = 3;

    QMap<QString, QDockWidget*> m_docks;
    QStringList                 m_dockOrder;
    QDockWidget*                m_firstDock = nullptr;
    QPointer<QDockWidget>       m_phantomDock;
    bool                        m_sideBySide = false;
    QQuickWidget*               m_welcomeWidget = nullptr;
};
