#ifndef MDIVIEW_H
#define MDIVIEW_H

#include <QWidget>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolBar>
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QTabBar>
#include <QToolButton>

class QHideEvent;
class QShowEvent;
class MainUIBackend;

class MdiView : public QWidget
{
    Q_OBJECT

public:
    explicit MdiView(MainUIBackend* backend = nullptr, QWidget *parent = nullptr);
    ~MdiView();
    
    // Add a plugin widget as an MDI window
    QMdiSubWindow* addPluginWindow(QWidget* pluginWidget, const QString& title);
    
    // Remove a plugin window
    void removePluginWindow(QWidget* pluginWidget);
    
    // Activate a plugin window by widget (bring to front)
    void activatePluginWindow(QWidget* pluginWidget);
    
    // Get widget for a plugin window (reverse lookup)
    QWidget* getWidgetForSubWindow(QMdiSubWindow* subWindow);

signals:
    void pluginWindowClosed(const QString& pluginName);

private slots:
    void addMdiWindow();
    void toggleViewMode();
    void updateTabCloseButtons();

private:
    void setupUi();
    void updateQmlPluginActiveStates();

    void ensureMdiAddButton(QTabBar* tabBar);
    void repositionMdiAddButton();

    void customizeTabBarStyle(QTabBar* tabBar);
    void installTabBarCloseButtons(QTabBar* tabBar);
    void insetTabBarGeometry(QTabBar *tabBar, int insetPx);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

    MainUIBackend* m_backend;
    QMdiArea *mdiArea;
    QPushButton *addButton;
    QPushButton *toggleButton;
    QToolBar *toolBar;
    QVBoxLayout *mainLayout;
    QToolButton* m_mdiAddBtn;

    // Map to keep track of plugin widgets and their MDI windows
    QMap<QWidget*, QMdiSubWindow*> m_pluginWindows;
    // Reverse map: subwindow -> widget
    QMap<QMdiSubWindow*, QWidget*> m_subWindowToWidget;
    // Per-subwindow connection handles for explicit disconnect in destructor.
    // Entries are removed when the subwindow is destroyed or removed.
    QMap<QMdiSubWindow*, QMetaObject::Connection> m_subWindowConnections;
    QMetaObject::Connection m_tabChangedConnection;
    
    int windowCounter;
};

#endif // MDIVIEW_H 
