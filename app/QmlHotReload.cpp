#include "QmlHotReload.h"

#include "LogosQmlBridge.h"

#include <QDebug>
#include <QDirIterator>
#include <QFileSystemWatcher>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickWidget>
#include <QVBoxLayout>

namespace {
constexpr int kDebounceMs = 150;
}

QmlHotReloadView::QmlHotReloadView(const QString& moduleName, const QString& sourceDir,
                                   const QUrl& entry, LogosQmlBridge* bridge,
                                   ViewFactory makeView, QWidget* parent)
    : QWidget(parent)
    , m_moduleName(moduleName)
    , m_sourceDir(sourceDir)
    , m_entry(entry)
    , m_bridge(bridge)
    , m_makeView(std::move(makeView))
    , m_watcher(new QFileSystemWatcher(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &QmlHotReloadView::reload);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));

    rewatch();
    qInfo().noquote() << "Hot reload:" << m_moduleName << "watching" << m_sourceDir
                      << "(LOGOS_QML_HOT_RELOAD=0 disables)";
}

bool QmlHotReloadView::load()
{
    // The old view and its engine go first; see the class comment. The bridge
    // must survive them, and the old engine's intent callbacks must not.
    bool wasActive = true;
    if (m_view) {
        wasActive = m_view->rootContext()->contextProperty(QStringLiteral("isActiveTab")).toBool();
        if (m_bridge) {
            m_bridge->abandonPendingIntents();
            m_bridge->setParent(this);
        }
        delete m_view;
        m_view = nullptr;
    }

    m_view = m_makeView();
    m_view->rootContext()->setContextProperty(QStringLiteral("isActiveTab"), wasActive);
    // The engine, not the widget, owns the bridge (see PluginLoader::finishUiQmlLoad).
    if (m_bridge) m_bridge->setParent(m_view->engine());
    layout()->addWidget(m_view);
    m_view->setSource(m_entry);
    emit viewReplaced(m_view);

    if (m_view->status() == QQuickWidget::Error) {
        qWarning().noquote() << "Hot reload:" << m_moduleName << "failed to load — fix and save again";
        for (const QQmlError& error : m_view->errors())
            qWarning().noquote() << "   " << error.toString();
        return false;
    }
    if (m_bridge) m_bridge->replayViewModuleState();
    return true;
}

void QmlHotReloadView::reload()
{
    rewatch();
    if (load()) qInfo().noquote() << "Hot reload:" << m_moduleName << "reloaded";
}

// Editors save by replacing the file, which drops it from the watcher, and new
// files and dirs need watching too, so the list is rebuilt before every reload.
void QmlHotReloadView::rewatch()
{
    QStringList wanted{m_sourceDir};
    QDirIterator dirs(m_sourceDir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (dirs.hasNext()) wanted << dirs.next();
    QDirIterator files(m_sourceDir, {"*.qml", "*.js", "*.mjs"}, QDir::Files,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) wanted << files.next();

    const QStringList previous = m_watcher->files() + m_watcher->directories();
    if (!previous.isEmpty()) m_watcher->removePaths(previous);
    m_watcher->addPaths(wanted);
}
