#pragma once

#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QStringList>

class QDockWidget;
class QTabBar;
class QToolButton;
class QHideEvent;
class QShowEvent;

// WorkspaceArea — Uses Qt's native tabifyDockWidget so dragging tabs 
// to detach + dropping docks onto tab strips both work for free; 
// reuses MdiView's tab styling to keep the look identical.
//
// Public API mirrors MdiView's:
//   * addPluginDock(widget, name) — first dock anchors the area, every
//     subsequent dock tabifies with it.
//   * removePluginDock(name) — tears the dock down.
//   * activatePluginDock(name) — raises the dock to the front of its tab.
//
// Emits pluginClosed(name) when the user clicks × on a tab; consumers
// wire this to backend.onPluginWindowClosed → unloadUiModule.
class WorkspaceArea : public QMainWindow
{
    Q_OBJECT

public:
    explicit WorkspaceArea(QWidget* parent = nullptr);
    ~WorkspaceArea() override;

    void addPluginDock(QWidget* pluginWidget, const QString& name);
    void removePluginDock(const QString& name);
    void activatePluginDock(const QString& name);
    void removePluginDock(QWidget* pluginWidget);
    void activatePluginDock(QWidget* pluginWidget);

    QDockWidget* dockFor(const QString& name) const { return m_docks.value(name); }
    QString nameForWidget(QWidget* w) const;

signals:
    void pluginClosed(const QString& name);

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

    // DEV: Ctrl+Shift+L toggles between tabbed and side-by-side.
    void toggleLayoutModeForTesting();

    QMap<QString, QDockWidget*> m_docks;
    QStringList                 m_dockOrder;   // insertion order
    QDockWidget*                m_firstDock = nullptr;
    QSet<QTabBar*>              m_styledTabBars;
    bool                        m_sideBySide = false;
};
