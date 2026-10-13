#pragma once

#include <QHash>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <functional>

class QFileSystemWatcher;
class QQuickWidget;
class LogosQmlBridge;

// Standalone --qml-source: an app's view, loaded from its source tree and
// rebuilt on every save. The tab holds this widget; the QQuickWidget inside it
// is replaced on each reload.
//
// A reload deletes the old QQuickWidget, and with it its engine, BEFORE the new
// one is built. Qt shares compiled QML types and scripts across engines while
// any engine still holds them, and a surviving engine keeps `.pragma library`
// scripts and `.mjs` modules alive even through clearComponentCache() — so
// reloading in place, or building the new engine first, replays old code.
//
// The backend is untouched: it lives in ui-host, and the bridge moves to the
// new engine (LogosQmlBridge supports exactly this; see replayViewModuleState).
class QmlHotReloadView : public QWidget {
    Q_OBJECT

public:
    // Builds an empty, sandboxed QQuickWidget for this app, with `logos` set.
    using ViewFactory = std::function<QQuickWidget*()>;

    QmlHotReloadView(const QString& moduleName, const QString& sourceDir,
                     const QUrl& entry, LogosQmlBridge* bridge, ViewFactory makeView,
                     QWidget* parent = nullptr);

    // The current view; changes on every reload.
    QQuickWidget* quickWidget() const { return m_view; }

    // Loads the entry into a fresh view. False if it did not compile; the
    // errors are logged and the next save tries again.
    bool load();

signals:
    void viewReplaced(QQuickWidget* view);

private:
    using Snapshot = QHash<QString, QPair<qint64, qint64>>;   // path -> (mtime, size)

    void onSourcesTouched();
    Snapshot snapshot() const;
    void reload();
    void rewatch();

    QString m_moduleName;
    QString m_sourceDir;
    QUrl m_entry;
    QPointer<LogosQmlBridge> m_bridge;
    ViewFactory m_makeView;
    QQuickWidget* m_view = nullptr;
    QFileSystemWatcher* m_watcher = nullptr;
    QTimer m_debounce;
    Snapshot m_snapshot;
};
