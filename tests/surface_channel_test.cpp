// srcdeps: surface/LogosSurfaceItem.cpp surface/SurfaceBackend.cpp surface/SurfaceTransportSelector.cpp surface/SurfaceChannelHost.cpp
//
// RFC-7 in Basecamp: the Logos.Surface item on the channel transport, end to
// end with a REAL view process. The view is this same binary re-run with
// --channel-view <token> <server>: a QGuiApplication that calls the RFC-8 C
// API (logos_view_surface_init) exactly as ui-host does, puts a coloured
// QQuickPaintedItem on LogosViewSurface::rootItem(), and reports its size and
// every press on stdout. The host runs under the offscreen QPA with the
// software scene graph and LOGOS_SURFACE_TRANSPORT=shm, which is the
// configuration every OS has (RFC-6 §3: channel-shm is the floor).
//
// Built only where the runtime's LogosViewSurface library is available
// (LOGOS_VIEW_SURFACE_ROOT); tests/CMakeLists.txt skips it otherwise.

#include "surface/LogosSurfaceItem.h"
#include "surface/SurfaceBackend.h"
#include "surface/SurfaceChannelHost.h"
#include "surface/SurfaceTransportSelector.h"

#include <SurfaceClientApi.h>
#include <SurfaceHostSession.h>
#include <logos/view_surface.h>

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdio>

namespace {

// ------------------------------------------------------------------ view ---

void say(const QString& line)
{
    fprintf(stdout, "view: %s\n", qPrintable(line));
    fflush(stdout);
}

class Probe : public QQuickPaintedItem {
public:
    Probe()
    {
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
        setFlag(ItemAcceptsInputMethod, true);
    }
    void paint(QPainter* p) override { p->fillRect(boundingRect(), QColor(0x33, 0x66, 0x99)); }

protected:
    void geometryChange(const QRectF& n, const QRectF& o) override
    {
        QQuickPaintedItem::geometryChange(n, o);
        if (n.size() != o.size()) say(QStringLiteral("SIZE %1 %2").arg(n.width()).arg(n.height()));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        say(QStringLiteral("PRESS %1 %2").arg(e->position().x()).arg(e->position().y()));
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        say(QStringLiteral("RELEASE %1 %2").arg(e->position().x()).arg(e->position().y()));
    }
    void wheelEvent(QWheelEvent* e) override
    {
        say(QStringLiteral("WHEEL %1").arg(e->angleDelta().y()));
        e->accept();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        say(QStringLiteral("KEY %1").arg(e->text()));
        e->accept();
    }
};

int runView(int argc, char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    const QByteArray token = argv[2];
    const QByteArray server = argv[3];
    if (logos_view_surface_init(token.constData(), server.constData(), "channel") != 0) {
        say(QStringLiteral("INIT_FAILED"));
        return 2;
    }
    QQuickWindow* win = LogosViewSurface::window();
    QQuickItem* root = LogosViewSurface::rootItem();
    if (!win || !root) { say(QStringLiteral("NO_WINDOW")); return 2; }
    auto* probe = new Probe;
    probe->setParentItem(root);
    probe->setSize(root->size());
    QObject::connect(root, &QQuickItem::widthChanged, probe, [probe, root] { probe->setWidth(root->width()); });
    QObject::connect(root, &QQuickItem::heightChanged, probe, [probe, root] { probe->setHeight(root->height()); });
    probe->forceActiveFocus();
    int frames = 0;
    QObject::connect(win, &QQuickWindow::afterRendering, &app, [&frames] {
        say(QStringLiteral("RENDERED %1").arg(++frames));
    });
    // Continuous demand, like a map that keeps animating: the host must keep
    // the credits flowing while it displays (RFC-7 §5), not stall after the
    // ring's first three frames.
    auto* animate = new QTimer(&app);
    animate->setInterval(16);
    QObject::connect(animate, &QTimer::timeout, probe, [probe] { probe->update(); });
    animate->start();
    QObject::connect(win, &QQuickWindow::screenChanged, &app, [win] {
        say(QStringLiteral("DPR %1").arg(win->effectiveDevicePixelRatio()));
    });
    say(QStringLiteral("SIZE %1 %2").arg(root->width()).arg(root->height()));
    auto* poll = new QTimer(&app);
    poll->setInterval(10);
    QObject::connect(poll, &QTimer::timeout, &app, [poll] {
        const QString t = LogosViewSurface::transport();
        if (t.startsWith(QLatin1String("channel-"))) {
            say(QStringLiteral("TRANSPORT %1").arg(t));
            poll->stop();
        }
    });
    poll->start();
    const int rc = app.exec();
    logos_view_surface_shutdown();
    return rc;
}

// ------------------------------------------------------------------ host ---

QString newToken()
{
    QString t;
    while (t.size() < 32) t += QString::number(QRandomGenerator::global()->generate(), 16);
    return t.left(32);
}

struct View {
    QProcess process;
    qint64 pid = 0;
    QStringList lines;

    void start(const QString& server, const QString& token)
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
        env.remove(QStringLiteral("QT_SCALE_FACTOR"));
        process.setProcessEnvironment(env);
        process.setProcessChannelMode(QProcess::MergedChannels);
        static const bool verbose = qEnvironmentVariableIsSet("LOGOS_TEST_VERBOSE");
        QObject::connect(&process, &QProcess::readyRead, &process, [this] {
            while (process.canReadLine()) {
                const QString line = QString::fromUtf8(process.readLine()).trimmed();
                if (verbose) fprintf(stderr, "  [view %lld] %s\n", (long long)pid, qPrintable(line));
                const int at = line.indexOf(QLatin1String("view: "));
                if (at >= 0) lines << line.mid(at + 6);
            }
        });
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--channel-view"), token, server});
        process.waitForStarted(5000);
        pid = process.processId();
    }
    bool has(const QString& prefix) const
    {
        for (const QString& l : lines)
            if (l.startsWith(prefix)) return true;
        return false;
    }
    QString last(const QString& kind) const
    {
        for (int i = lines.size() - 1; i >= 0; --i)
            if (lines[i].startsWith(kind + QLatin1Char(' '))) return lines[i];
        return QString();
    }
    void stop()
    {
        if (process.state() == QProcess::NotRunning) return;
        process.terminate();
        if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(1000); }
    }
};

const char* kHostQml = R"(
    import QtQuick
    import Logos.Surface
    Item {
        width: 400; height: 300
        Surface {
            id: surface
            objectName: "surface"
            anchors.fill: parent
            module: "fixture"
            session: "test"
            property var errors: []
            onError: (code) => errors.push(code)
        }
    }
)";

} // namespace

class SurfaceChannelTest : public QObject {
    Q_OBJECT

    SurfaceChannelHost* m_host = nullptr;

    QQuickWidget* makeHost(const QString& boundAs)
    {
        auto* widget = new QQuickWidget;
        widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        LogosSurfaceItem::bindEngine(widget->engine(), boundAs);
        auto* component = new QQmlComponent(widget->engine(), widget);
        component->setData(kHostQml, QUrl(QStringLiteral("host.qml")));
        if (component->isError())
            for (const auto& e : component->errors()) qWarning() << e.toString();
        widget->setContent(QUrl(), component, component->create());
        widget->resize(400, 300);
        widget->show();
        return widget;
    }

    static LogosSurfaceItem* surfaceOf(QQuickWidget* w)
    {
        return w->rootObject()->findChild<LogosSurfaceItem*>(QStringLiteral("surface"));
    }

    // What PluginLoader does around a spawn (RFC-5 Part B, RFC-7 §3).
    QString openAndRecord(const QString& module)
    {
        const QString server = m_host->open(module);
        SurfaceTransportSelector::record(module, QStringLiteral("channel"));
        return server;
    }
    void forgetAll(const QString& module)
    {
        m_host->forget(module);
        SurfaceTransportSelector::forget(module);
    }

private slots:
    void initTestCase()
    {
        qmlRegisterType<LogosSurfaceItem>("Logos.Surface", 1, 0, "Surface");
        const SurfaceTransportSelector::Selection sel = SurfaceTransportSelector::select();
        QCOMPARE(sel.family, QStringLiteral("channel"));
        QVERIFY(sel.shmOnly);
        m_host = SurfaceChannelHost::instance();
        QVERIFY(m_host);
        QCOMPARE(SurfaceChannelHost::existing(), m_host);
    }

    // The happy path: a frame over channel-shm, host-driven size at DPR 2,
    // a press delivered in the view's coordinates, a quiet forget().
    void boundSizedAndPressed()
    {
        const QString server = openAndRecord(QStringLiteral("fixture"));
        QVERIFY(!server.isEmpty());
        View view;
        const QString token = newToken();
        view.start(server, token);
        QVERIFY(view.pid > 0);
        m_host->expect(QStringLiteral("fixture"), token, view.pid);

        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());
        QVERIFY(item);
        QVERIFY(item->backend());
        QCOMPARE(item->backend()->family(), QStringLiteral("channel"));
        QTRY_VERIFY_WITH_TIMEOUT(item->isReady(), 10000);
        QCOMPARE(item->transport(), QStringLiteral("channel-shm"));
        QCOMPARE(item->viewId().section(QLatin1Char('/'), 0, 1), QStringLiteral("fixture/test"));
        QTRY_VERIFY_WITH_TIMEOUT(view.has(QStringLiteral("TRANSPORT channel-shm")), 5000);

        // Host-driven size: the view's root is 400x300 logical...
        QTRY_COMPARE_WITH_TIMEOUT(view.last(QStringLiteral("SIZE")), QStringLiteral("SIZE 400 300"), 5000);
        // ...and the slot ring holds 800x600 pixels at the host's DPR 2.
        logos::surface::SurfaceHostSession* session = m_host->sessionFor(QStringLiteral("fixture"));
        QVERIFY(session);
        QCOMPARE(session->pixelSize(), QSize(800, 600));

        // The view keeps animating; with the host displaying, frames keep
        // coming well past the ring's three slots.
        auto rendered = [&view] { return view.last(QStringLiteral("RENDERED")).section(QLatin1Char(' '), 1).toInt(); };
        QTRY_VERIFY2_WITH_TIMEOUT(rendered() >= 12, qPrintable(QStringLiteral("rendered %1").arg(rendered())), 5000);

        // A press on the item reaches the view in its own logical coordinates.
        QTest::mouseClick(host.data(), Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
        QTRY_VERIFY_WITH_TIMEOUT(view.has(QStringLiteral("PRESS")), 5000);
        const QStringList press = view.last(QStringLiteral("PRESS")).split(QLatin1Char(' '));
        QCOMPARE(press.size(), 3);
        QVERIFY2(qAbs(press[1].toDouble() - 200) <= 2, qPrintable(view.last(QStringLiteral("PRESS"))));
        QVERIFY2(qAbs(press[2].toDouble() - 150) <= 2, qPrintable(view.last(QStringLiteral("PRESS"))));
        QTRY_VERIFY_WITH_TIMEOUT(view.has(QStringLiteral("RELEASE")), 5000);

        // Resize: a configure in logical pixels, a new generation at scale.
        host->resize(200, 150);
        QTRY_COMPARE_WITH_TIMEOUT(view.last(QStringLiteral("SIZE")), QStringLiteral("SIZE 200 150"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(session->pixelSize(), QSize(400, 300), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(item->isReady(), 5000);

        // Unload order (RFC-5 rule 4): forget, then stop. No error on the item.
        QSignalSpy closed(m_host, &SurfaceChannelHost::sessionClosed);
        forgetAll(QStringLiteral("fixture"));
        QCOMPARE(closed.count(), 1);
        view.stop();
        QTest::qWait(200);
        QVERIFY(item->property("errors").toList().isEmpty());
    }

    // A view that dies on its own: the last frame stays, the item reports it.
    void viewExitIsReported()
    {
        const QString server = openAndRecord(QStringLiteral("fixture"));
        View view;
        const QString token = newToken();
        view.start(server, token);
        m_host->expect(QStringLiteral("fixture"), token, view.pid);
        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());
        QTRY_VERIFY_WITH_TIMEOUT(item->isReady(), 10000);

        logos::surface::SurfaceHostSession* session = m_host->sessionFor(QStringLiteral("fixture"));
        QVERIFY(session);
        QTRY_VERIFY_WITH_TIMEOUT(session->displayedSlot() != nullptr, 5000);

        view.process.kill();
        view.process.waitForFinished(3000);
        QTRY_VERIFY_WITH_TIMEOUT(item->property("errors").toList().contains(QStringLiteral("clientExited")), 5000);
        // Same contract as the Wayland backend: not ready, transport none...
        QVERIFY(!item->isReady());
        QCOMPARE(item->transport(), QStringLiteral("none"));
        // ...but the last frame is kept (RFC-7 §5): the displayed slot
        // survives the disconnect and the backend goes on painting it.
        QVERIFY(session->displayedSlot() != nullptr);
        QVERIFY(!session->isConnected());
        forgetAll(QStringLiteral("fixture"));
    }

    // Admission: a view presenting the wrong token is dropped; the item
    // never becomes ready.
    void wrongTokenIsRefused()
    {
        const QString server = openAndRecord(QStringLiteral("fixture"));
        View view;
        view.start(server, newToken());
        m_host->expect(QStringLiteral("fixture"), newToken(), view.pid);
        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());
        QTest::qWait(1500);
        QVERIFY(!item->isReady());
        QVERIFY(!view.has(QStringLiteral("TRANSPORT")));
        forgetAll(QStringLiteral("fixture"));
        view.stop();
    }

    // Sandbox invariants hold on this transport too: the wrong module gets
    // tokenMismatch; a module without a session gets notGuiModule.
    void itemRefusesOtherModules()
    {
        {
            QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("evil")));
            LogosSurfaceItem* item = surfaceOf(host.data());
            QTRY_COMPARE_WITH_TIMEOUT(item->property("errors").toList(),
                                      QVariantList{QStringLiteral("tokenMismatch")}, 2000);
            QCOMPARE(item->transport(), QStringLiteral("none"));
        }
        {
            QVERIFY(!m_host->hasSession(QStringLiteral("fixture")));
            QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
            LogosSurfaceItem* item = surfaceOf(host.data());
            QTRY_COMPARE_WITH_TIMEOUT(item->property("errors").toList(),
                                      QVariantList{QStringLiteral("notGuiModule")}, 2000);
        }
    }
};

int main(int argc, char** argv)
{
    if (argc >= 4 && qstrcmp(argv[1], "--channel-view") == 0)
        return runView(argc, argv);

    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_SCALE_FACTOR", "2");
    qputenv("LOGOS_SURFACE_TRANSPORT", "shm");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QApplication app(argc, argv);
    SurfaceChannelTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "surface_channel_test.moc"
