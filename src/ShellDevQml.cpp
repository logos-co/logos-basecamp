#include "ShellDevQml.h"

#include <QDebug>
#include <QDir>
#include <QDateTime>
#include <QDirIterator>
#include <QHash>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWidget>
#include <QTimer>

#include <memory>
#include <vector>

namespace ShellDevQml {

namespace {

constexpr int kDebounceMs = 150;

QString devRoot()
{
    static const QString root = [] {
        const QString dev = QString::fromUtf8(qgetenv("DEV_QML_PATH")).trimmed();
        if (dev.isEmpty()) return QString();
        if (!QFileInfo(dev).isDir()) {
            qWarning().noquote() << "DEV_QML_PATH is not a directory:" << dev
                                 << "- using embedded QML";
            return QString();
        }
        return QDir(dev).canonicalPath();
    }();
    return root;
}

// Bumped on every reload. Each source file is loaded as <file>?reload=<n>, so
// after a reload no engine or cache can hand back an old copy: roots, sibling
// files and module types alike get a URL never seen before.
int g_generation = 0;

QUrl withGeneration(const QString& file)
{
    QUrl url = QUrl::fromLocalFile(file);
    if (g_generation > 0) url.setQuery(QStringLiteral("reload=%1").arg(g_generation));
    return url;
}

QUrl toSource(const QUrl& url)
{
    static const QString prefix = QStringLiteral("/qt/qml/Basecamp/");
    if (url.isLocalFile()) {
        const QString file = url.toLocalFile();
        return file.startsWith(devRoot() + QLatin1Char('/')) ? withGeneration(file) : url;
    }
    if (url.scheme() != QLatin1String("qrc") || !url.path().startsWith(prefix))
        return url;
    const QString rest = url.path().mid(prefix.size());   // <Mod>/<source path>
    const int slash = rest.indexOf(QLatin1Char('/'));
    if (slash < 0) return url;
    const QString file = QDir(devRoot()).filePath(rest.mid(slash + 1));
    return QFileInfo(file).isFile() ? withGeneration(file) : url;
}

class SourceInterceptor : public QQmlAbstractUrlInterceptor {
public:
    QUrl intercept(const QUrl& url, DataType type) override
    {
        if (type != QmlFile && type != JavaScriptFile) return url;
        return toSource(url);
    }
};

class Reloader : public QObject {
public:
    static Reloader& instance()
    {
        static auto* reloader = new Reloader;
        return *reloader;
    }

    void add(QQuickWidget* view, const QUrl& url, std::function<void(QObject*)> wire)
    {
        m_views.push_back({view, url, std::move(wire)});
        if (!m_watcher) start();
    }

private:
    struct View {
        QPointer<QQuickWidget> widget;
        QUrl url;
        std::function<void(QObject*)> wire;
    };

    void start()
    {
        m_watcher = new QFileSystemWatcher(this);
        m_debounce.setSingleShot(true);
        m_debounce.setInterval(kDebounceMs);
        connect(&m_debounce, &QTimer::timeout, this, &Reloader::onSourcesTouched);
        connect(m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
        rewatch();
        qInfo().noquote() << "Shell hot reload: watching" << devRoot()
                          << "(LOGOS_QML_HOT_RELOAD=0 disables)";
    }

    using Snapshot = QHash<QString, QPair<qint64, qint64>>;   // path -> (mtime, size)

    void onSourcesTouched()
    {
        if (snapshot() == m_snapshot) return;
        reload();
    }

    Snapshot snapshot() const
    {
        Snapshot snap;
        QDirIterator files(devRoot(), {"*.qml", "*.js", "*.mjs"}, QDir::Files,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QFileInfo info(files.next());
            snap.insert(info.filePath(), {info.lastModified().toMSecsSinceEpoch(), info.size()});
        }
        return snap;
    }

    // Editors save by replacing the file, which drops it from the watcher, and
    // new files need watching too, so the list is rebuilt before every reload.
    void rewatch()
    {
        m_snapshot = snapshot();
        QStringList wanted = m_snapshot.keys();
        wanted << devRoot();
        QDirIterator dirs(devRoot(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (dirs.hasNext()) wanted << dirs.next();

        const QStringList previous = m_watcher->files() + m_watcher->directories();
        if (!previous.isEmpty()) m_watcher->removePaths(previous);
        if (!wanted.isEmpty()) m_watcher->addPaths(wanted);
    }

    // Empty every view; once all the old trees are gone, clear every cache,
    // then load again. A tree may go synchronously or by deleteLater, so the
    // count is taken before any view is emptied.
    void reload()
    {
        rewatch();
        auto pending = std::make_shared<int>(0);
        std::vector<QPointer<QObject>> roots;
        for (View& v : m_views)
            if (v.widget && v.widget->rootObject()) roots.emplace_back(v.widget->rootObject());
        for (const QPointer<QObject>& root : roots) {
            ++*pending;
            connect(root, &QObject::destroyed, this, [this, pending] {
                if (--*pending == 0)
                    QMetaObject::invokeMethod(this, [this] { reloadNow(); }, Qt::QueuedConnection);
            });
        }
        for (View& v : m_views)
            if (v.widget) v.widget->setSource(QUrl());
        if (roots.empty()) reloadNow();
    }

    void reloadNow()
    {
        for (View& v : m_views)
            if (v.widget) v.widget->engine()->clearComponentCache();

        ++g_generation;
        bool ok = true;
        for (View& v : m_views) {
            if (!v.widget) continue;
            v.widget->setSource(toSource(v.url));
            if (v.widget->status() == QQuickWidget::Error) {
                ok = false;
                for (const QQmlError& error : v.widget->errors())
                    qWarning().noquote() << "   " << error.toString();
                continue;
            }
            if (v.wire && v.widget->rootObject()) v.wire(v.widget->rootObject());
        }
        if (ok) qInfo().noquote() << "Shell hot reload: reloaded";
        else qWarning().noquote() << "Shell hot reload: failed to load — fix and save again";
    }

    std::vector<View> m_views;
    Snapshot m_snapshot;
    QFileSystemWatcher* m_watcher = nullptr;
    QTimer m_debounce;
};

} // namespace

void load(QQuickWidget* view, const QUrl& qrcUrl, std::function<void(QObject* root)> wire)
{
    if (devRoot().isEmpty()) {
        view->setSource(qrcUrl);
    } else {
        static SourceInterceptor interceptor;   // stateless; shared by every shell engine
        view->engine()->addUrlInterceptor(&interceptor);
        const QUrl source = toSource(qrcUrl);
        qInfo().noquote() << "DEV_QML_PATH override active:" << source.toString();
        view->setSource(source);
        if (qgetenv("LOGOS_QML_HOT_RELOAD") != "0")
            Reloader::instance().add(view, qrcUrl, wire);
    }
    if (wire && view->rootObject()) wire(view->rootObject());
}

} // namespace ShellDevQml
