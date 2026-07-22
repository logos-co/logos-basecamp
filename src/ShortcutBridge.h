#pragma once

#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <functional>

class QQuickWidget;
class QShortcut;
class QStackedWidget;
class QWidget;

// Mirrors QML `Shortcut { }` blocks inside the current pane's QQuickWidget
// as real C++ QShortcuts on the host, so they fire despite the offscreen
// QQuickWindow limitation. Plugin authors write ordinary QML — the bridge
// does the rest automatically on pane switch.
class ShortcutBridge : public QObject {
    Q_OBJECT
public:
    // Returns an additional QQuickWidget to scan when the current stack
    // widget isn't itself one (e.g. WorkspaceArea's active dock).
    using DockPaneProvider = std::function<QQuickWidget*()>;

    ShortcutBridge(QWidget* host,
                   QStackedWidget* stack,
                   DockPaneProvider dockProvider = {});

    // Re-scan the current pane. Auto-called on stack switch; call
    // manually when new QML materialises (plugin arrival, dock swap).
    void rebindDeferred();

private slots:
    void onWiredShortcut();

private:
    void rebind();
    void clearMirrors();
    // Scan a pane's QML tree and mirror its Shortcuts. Returns false if
    // the QML wasn't Ready yet (a statusChanged hook was installed).
    bool scanPane(QQuickWidget* pane);
    void mirrorOneShortcut(QObject* qmlShortcut);

    QWidget*                             m_host;
    QStackedWidget*                      m_stack;
    DockPaneProvider                     m_dockProvider;

    QList<QPointer<QShortcut>>           m_mirrors;
    QHash<QObject*, QPointer<QObject>>   m_mirrorToQml;
    QList<QMetaObject::Connection>       m_transientConns;
};
