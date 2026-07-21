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

public:
    explicit WorkspaceArea(QObject* backend = nullptr, QWidget* parent = nullptr);
    ~WorkspaceArea() override;

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

signals:
    void pluginClosed(const QString& moduleName);
    void installClicked();

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
