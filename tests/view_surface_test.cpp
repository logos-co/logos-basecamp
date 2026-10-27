// srcdeps: surface/SurfaceCompositor.cpp surface/LogosSurfaceItem.cpp surface/SurfaceBackend.cpp surface/WaylandSurfaceBackend.cpp surface/SurfaceTransportSelector.cpp
// libdeps: Qt6::WaylandCompositor
//
// RFC-4 §Tests / maps architecture §16.6: the nested compositor and the
// Logos.Surface item, end to end with a REAL Wayland client. The client is
// this same binary re-run with --surface-client <token>: a QGuiApplication on
// the wayland QPA (software Quick backend, shm buffers) that opens one window
// filled with a colour and reports its logical size and every touch point on
// stdout. No fixture module and no `qml` tool needed, so the check runs in the
// dependency-free unit-test harness.
//
// The host runs under the offscreen QPA with QT_SCALE_FACTOR=2 and the software
// scene graph: a 400x300 logical item must produce an 800x600 client buffer.

#include "surface/LogosSurfaceItem.h"
#include "surface/SurfaceCompositor.h"

#include <QApplication>
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTouchEvent>
#include <QWaylandOutput>
#include <QWaylandSurface>
#include <QWaylandView>
#include <QWaylandXdgShell>
#include <QtTest>

#include <cstdio>

#ifdef LOGOS_HAVE_RAW_WAYLAND_CLIENT
#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

// ------------------------------------------------------------ raw client ---
//
// RFC-4 rev 2 §Tests: a client that speaks libwayland directly, so it can do
// what no Qt client would — list and try to bind hidden globals (H2), commit
// an oversize buffer (H3), and truncate its shm pool behind a committed
// buffer (H1). Re-run of this binary: --raw-client <token> <probe|oversize|truncate>.
// Reports on stdout as "raw: KIND ..." lines.

#ifdef LOGOS_HAVE_RAW_WAYLAND_CLIENT

struct Raw {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm = nullptr;
    QStringList globals;
    bool configured = false;
    bool framed = false;
};

void rawSay(const char* kind, const QString& rest = QString())
{
    fprintf(stdout, "raw: %s %s\n", kind, qPrintable(rest));
    fflush(stdout);
}

void onGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    auto* raw = static_cast<Raw*>(data);
    raw->globals << QString::fromLatin1(interface);
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        raw->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, qMin(4u, version)));
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        raw->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        raw->wm = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
}
void onGlobalRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener = {onGlobal, onGlobalRemove};

void onPing(void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }
const xdg_wm_base_listener kWmListener = {onPing};

void onXdgConfigure(void* data, xdg_surface* surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
    static_cast<Raw*>(data)->configured = true;
}
const xdg_surface_listener kXdgSurfaceListener = {onXdgConfigure};

void onToplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void onToplevelClose(void*, xdg_toplevel*) {}
void onToplevelBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void onToplevelCaps(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kToplevelListener = {onToplevelConfigure, onToplevelClose,
                                                 onToplevelBounds, onToplevelCaps};

void onFrame(void* data, wl_callback* cb, uint32_t)
{
    static_cast<Raw*>(data)->framed = true;
    wl_callback_destroy(cb);
}
const wl_callback_listener kFrameListener = {onFrame};

// Roundtrips until the connection is gone; reports how it went.
int rawDrain(Raw& raw)
{
    for (int i = 0; i < 200; ++i) {
        if (wl_display_roundtrip(raw.display) < 0) {
            const int err = wl_display_get_error(raw.display);
            rawSay("DISCONNECTED", QString::number(err));
            return 0;
        }
        usleep(50 * 1000);
    }
    rawSay("STILL_CONNECTED");
    return 4;
}

int runRawClient(int argc, char** argv)
{
    Q_UNUSED(argc);
    const QString token = QString::fromLatin1(argv[2]);
    const QByteArray mode = argv[3];
    Raw raw;
    raw.display = wl_display_connect(nullptr);
    if (!raw.display) { rawSay("NO_DISPLAY"); return 2; }
    raw.registry = wl_display_get_registry(raw.display);
    wl_registry_add_listener(raw.registry, &kRegistryListener, &raw);
    wl_display_roundtrip(raw.display);
    for (const QString& g : std::as_const(raw.globals)) rawSay("GLOBAL", g);

    if (mode == "probe") {
        // A hidden global has no numeric name the registry told us about, so
        // guess: every bind with a wrong name or interface is a protocol error
        // and the server drops us. Either way nothing is obtained.
        rawSay("BIND_ATTEMPT", QStringLiteral("wl_data_device_manager"));
        for (uint32_t name = 1; name < 64; ++name) {
            wl_proxy* p = static_cast<wl_proxy*>(
                wl_registry_bind(raw.registry, name, &wl_data_device_manager_interface, 1));
            if (p) wl_proxy_destroy(p);
            if (wl_display_roundtrip(raw.display) < 0) break;
        }
        const int err = wl_display_get_error(raw.display);
        rawSay(err ? "BIND_ERROR" : "BIND_SILENT", QString::number(err));
        return 0;
    }

    if (!raw.compositor || !raw.shm || !raw.wm) { rawSay("MISSING_GLOBAL"); return 2; }
    xdg_wm_base_add_listener(raw.wm, &kWmListener, &raw);
    wl_surface* surface = wl_compositor_create_surface(raw.compositor);
    xdg_surface* xdg = xdg_wm_base_get_xdg_surface(raw.wm, surface);
    xdg_surface_add_listener(xdg, &kXdgSurfaceListener, &raw);
    xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(toplevel, &kToplevelListener, &raw);
    xdg_toplevel_set_app_id(toplevel, qPrintable(QStringLiteral("logos-view-") + token));
    wl_surface_commit(surface);
    for (int i = 0; i < 100 && !raw.configured; ++i)
        if (wl_display_roundtrip(raw.display) < 0) { rawSay("DISCONNECTED", "before configure"); return 3; }
    if (!raw.configured) { rawSay("NO_CONFIGURE"); return 3; }

    const int w = mode == "oversize" ? 9000 : 400;
    const int h = mode == "oversize" ? 9000 : 300;
    const int stride = w * 4;
    const off_t bytes = off_t(stride) * h;
    const int fd = memfd_create("logos-raw-pool", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, bytes) != 0) { rawSay("NO_POOL", QString::fromLatin1(strerror(errno))); return 2; }
    if (mode != "oversize") {
        // Fill with an opaque colour so the host has something to show.
        void* map = mmap(nullptr, size_t(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) { memset(map, 0x7f, size_t(bytes)); munmap(map, size_t(bytes)); }
    }
    wl_shm_pool* pool = wl_shm_create_pool(raw.shm, fd, int32_t(bytes));
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, w, h);
    wl_callback* frame = wl_surface_frame(surface);
    wl_callback_add_listener(frame, &kFrameListener, &raw);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(raw.display) < 0) { rawSay("DISCONNECTED", "at commit"); return 0; }
    rawSay("COMMITTED", QStringLiteral("%1x%2").arg(w).arg(h));

    if (mode == "oversize") return rawDrain(raw);

    // truncate: let the host show the buffer once, then pull the pages away
    // behind the buffer it still holds and commit again.
    for (int i = 0; i < 200 && !raw.framed; ++i) {
        if (wl_display_roundtrip(raw.display) < 0) { rawSay("DISCONNECTED", "before frame"); return 3; }
        usleep(20 * 1000);
    }
    rawSay(raw.framed ? "FRAME" : "NO_FRAME");
    if (ftruncate(fd, 0) != 0) { rawSay("NO_TRUNCATE"); return 2; }
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, w, h);
    wl_surface_commit(surface);
    wl_display_flush(raw.display);
    rawSay("TRUNCATED");
    return rawDrain(raw);
}
#endif // LOGOS_HAVE_RAW_WAYLAND_CLIENT

// ---------------------------------------------------------------- client ---

int runClient(int argc, char** argv)
{
    // What ui-host does for a --surface-socket spawn (RFC-1/2): wayland QPA,
    // app_id from the token, no client-side decoration. The environment
    // (WAYLAND_DISPLAY, QT_QPA_PLATFORM, QT_QUICK_BACKEND) comes from the host.
    const QString token = QString::fromLatin1(argv[2]);
    QGuiApplication app(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("logos-view-") + token);

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    QQmlComponent component(view.engine());
    component.setData(R"(
        import QtQuick
        Rectangle {
            color: "#2255cc"
            onWidthChanged: console.log("SIZE", width, height)
            onHeightChanged: console.log("SIZE", width, height)
            MultiPointTouchArea {
                anchors.fill: parent
                onPressed: (points) => { for (const p of points) console.log("TOUCH", p.x, p.y) }
            }
            Component.onCompleted: console.log("SIZE", width, height)
        }
    )", QUrl(QStringLiteral("client.qml")));
    if (component.isError()) {
        for (const auto& e : component.errors()) qWarning() << e.toString();
        return 2;
    }
    auto* root = qobject_cast<QQuickItem*>(component.create());
    if (!root) return 2;
    view.setContent(QUrl(), &component, root);
    // Same "qml: KIND ..." shape as console.log, so the host parses one format.
    auto report = [&view] {
        fprintf(stderr, "qml: DPR %g\n", view.devicePixelRatio());
        fflush(stderr);
    };
    QObject::connect(&view, &QWindow::screenChanged, &view, report);
    QObject::connect(view.screen(), &QScreen::physicalDotsPerInchChanged, &view, report);
    view.show();
    report();
    return app.exec();
}

// ------------------------------------------------------------------ host ---

QString newToken()
{
    QString t;
    while (t.size() < 32) t += QString::number(QRandomGenerator::global()->generate(), 16);
    return t.left(32);
}

struct Client {
    QProcess process;
    QString token;
    qint64 pid = 0;         // captured at start: processId() is 0 once it exits
    QStringList lines;      // stdout lines, as "KIND a b"
    QStringList raw;

    void start(const QString& socket, const QString& withToken)
    {
        token = withToken;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
        env.insert(QStringLiteral("WAYLAND_DISPLAY"), socket);
        env.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
        env.insert(QStringLiteral("QT_WAYLAND_DISABLE_WINDOWDECORATION"), QStringLiteral("1"));
        // A Qt built with journald logs there when stderr is not a tty.
        env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
        env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("qml.debug=true"));
        env.remove(QStringLiteral("QT_SCALE_FACTOR"));
        env.remove(QStringLiteral("DISPLAY"));
        // A Plasma session exports this; a refused client would then
        // reconnect forever instead of exiting (ui-host unsets it too).
        env.remove(QStringLiteral("QT_WAYLAND_RECONNECT"));
        process.setProcessEnvironment(env);
        process.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&process, &QProcess::readyRead, &process, [this] {
            while (process.canReadLine()) {
                const QString line = QString::fromUtf8(process.readLine()).trimmed();
                raw << line;
                // console.log from QML arrives as "qml: ..."
                const int at = line.indexOf(QLatin1String("qml: "));
                if (at >= 0) lines << line.mid(at + 5);
            }
        });
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--surface-client"), token});
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

// The raw libwayland client as a process; same stdout convention, "raw: ".
struct RawClient {
    QProcess process;
    qint64 pid = 0;
    QStringList lines;

    void start(const QString& socket, const QString& token, const QString& mode)
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("WAYLAND_DISPLAY"), socket);
        process.setProcessEnvironment(env);
        process.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&process, &QProcess::readyRead, &process, [this] {
            while (process.canReadLine()) {
                const QString line = QString::fromUtf8(process.readLine()).trimmed();
                const int at = line.indexOf(QLatin1String("raw: "));
                if (at >= 0) lines << line.mid(at + 5).trimmed();
            }
        });
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--raw-client"), token, mode});
        process.waitForStarted(5000);
        pid = process.processId();
    }
    bool has(const QString& prefix) const
    {
        for (const QString& l : lines)
            if (l.startsWith(prefix)) return true;
        return false;
    }
    QStringList globals() const
    {
        QStringList out;
        for (const QString& l : lines)
            if (l.startsWith(QLatin1String("GLOBAL "))) out << l.mid(7);
        return out;
    }
    bool finished() const { return process.state() == QProcess::NotRunning; }
    void stop()
    {
        if (finished()) return;
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

class ViewSurfaceTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_runtimeDir;
    SurfaceCompositor* m_compositor = nullptr;

    // A sandboxed-looking QQuickWidget: engine bound to module `boundAs`.
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

    static void touchAt(QQuickWidget* w, const QPointF& local)
    {
        static QPointingDevice device(QStringLiteral("test touch"), 1,
                                      QInputDevice::DeviceType::TouchScreen,
                                      QPointingDevice::PointerType::Finger,
                                      QInputDevice::Capability::Position, 10, 0);
        // Through the platform input path (QTest::touchEvent), so the
        // point reaches the QQuickWidget the way a real touchscreen's would.
        QTest::touchEvent(w, &device).press(1, local.toPoint(), w);
        QTest::touchEvent(w, &device).release(1, local.toPoint(), w);
        // QWaylandCompositor flushes clients when the event dispatcher is
        // about to block; QTest::qWait never blocks it, so flush here.
        SurfaceCompositor::instance()->compositor()->processWaylandEvents();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_runtimeDir.isValid());
        qputenv("XDG_RUNTIME_DIR", m_runtimeDir.path().toUtf8());
        qmlRegisterType<LogosSurfaceItem>("Logos.Surface", 1, 0, "Surface");
        m_compositor = SurfaceCompositor::instance();
        QVERIFY(m_compositor);
        QVERIFY2(m_compositor->isAvailable(), "nested compositor did not come up");
        QVERIFY(m_compositor->socketName().startsWith(QStringLiteral("logos-basecamp-")));
        QCOMPARE(SurfaceCompositor::existing(), m_compositor);
    }

    // The happy path: bound within 2 s, sized by the host, DPR 2 buffer,
    // touch delivered, shm transport under offscreen, nothing after forget().
    void boundSizedAndTouched()
    {
        // Production order (PluginLoader): spawn, expect, THEN the view's QML.
        Client client;
        const QString token = newToken();
        QSignalSpy bound(m_compositor, &SurfaceCompositor::surfaceBound);
        client.start(m_compositor->socketName(), token);
        QVERIFY(client.pid > 0);
        m_compositor->expect(QStringLiteral("fixture"), token, client.pid);
        QCOMPARE(m_compositor->tokenFor(QStringLiteral("fixture")), token);

        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());
        QVERIFY(item);
        QVERIFY(!item->isReady());

        QTRY_VERIFY_WITH_TIMEOUT(item->isReady(), 10000);
        QCOMPARE(bound.count(), 1);
        QCOMPARE(bound.first().first().toString(), token);
        QCOMPARE(item->viewId().section(QLatin1Char('/'), 0, 1), QStringLiteral("fixture/test"));
        // Known once the first buffer is current in the scene graph.
        QTRY_COMPARE_WITH_TIMEOUT(item->transport(), QStringLiteral("wayland-shm"), 5000);
        QCOMPARE(m_compositor->refusedCount(), 0);

        // Host-driven size: 400x300 logical at output scale 2 -> 800x600 buffer.
        QTRY_VERIFY2_WITH_TIMEOUT(client.last(QStringLiteral("SIZE")) == QStringLiteral("SIZE 400 300"),
                                  qPrintable(client.raw.join(QLatin1Char('|'))), 5000);
        QWaylandXdgSurface* xdg = m_compositor->boundSurface(token);
        QVERIFY(xdg);
        QTRY_VERIFY2_WITH_TIMEOUT(xdg->surface()->bufferSize() == QSize(800, 600),
                                  qPrintable(QStringLiteral("buffer %1x%2 scale %3; client: %4")
                                      .arg(xdg->surface()->bufferSize().width())
                                      .arg(xdg->surface()->bufferSize().height())
                                      .arg(xdg->surface()->bufferScale())
                                      .arg(client.raw.join(QLatin1Char('|')))), 5000);
        QCOMPARE(xdg->surface()->bufferScale(), 2);

        // Touch through the host scene reaches the client in its own
        // logical coordinates.
        QTest::qWait(100);
        touchAt(host.data(), QPointF(200, 150));
        QTRY_VERIFY_WITH_TIMEOUT(client.has(QStringLiteral("TOUCH")), 5000);
        const QStringList touch = client.last(QStringLiteral("TOUCH")).split(QLatin1Char(' '));
        QCOMPARE(touch.size(), 3);
        QVERIFY2(qAbs(touch[1].toDouble() - 200) <= 2, qPrintable(client.last(QStringLiteral("TOUCH"))));
        QVERIFY2(qAbs(touch[2].toDouble() - 150) <= 2, qPrintable(client.last(QStringLiteral("TOUCH"))));

        // Resize: a configure in logical pixels, a new buffer at scale.
        host->resize(200, 150);
        QTRY_COMPARE_WITH_TIMEOUT(client.last(QStringLiteral("SIZE")), QStringLiteral("SIZE 200 150"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(xdg->surface()->bufferSize(), QSize(400, 300), 5000);

        // Invisible item: off its output, so no frame callbacks (RFC-5 rule 1).
        item->setVisible(false);
        QTest::qWait(50);
        item->setVisible(true);
        QVERIFY(item->isReady());

        // Unload order (RFC-5 rule 4): forget, then stop. No lost surface, no
        // error on the item.
        QSignalSpy lost(m_compositor, &SurfaceCompositor::surfaceLost);
        m_compositor->forget(token);
        QVERIFY(!m_compositor->isExpected(token));
        QVERIFY(m_compositor->tokenFor(QStringLiteral("fixture")).isEmpty());
        client.stop();
        QTest::qWait(200);
        QCOMPARE(lost.count(), 0);
        QCOMPARE(item->property("errors").toList().size(), 0);
    }

    // A client that exits on its own: surfaceLost, and the item reports it.
    void clientExitIsReported()
    {
        Client client;
        const QString token = newToken();
        client.start(m_compositor->socketName(), token);
        m_compositor->expect(QStringLiteral("fixture"), token, client.pid);
        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());
        QTRY_VERIFY_WITH_TIMEOUT(item->isReady(), 10000);

        QSignalSpy lost(m_compositor, &SurfaceCompositor::surfaceLost);
        client.process.kill();
        client.process.waitForFinished(3000);
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!item->isReady(), 5000);
        QCOMPARE(item->property("errors").toList(), QVariantList{QStringLiteral("clientExited")});
        QCOMPARE(item->transport(), QStringLiteral("none"));
        m_compositor->forget(token);
    }

    // Admission: an expected pid presenting the wrong app_id is refused and
    // its connection closed; the item never binds.
    void wrongAppIdIsRefused()
    {
        Client client;
        const QString expected = newToken();
        const QString presented = newToken();
        QSignalSpy refused(m_compositor, &SurfaceCompositor::surfaceRefused);
        const int before = m_compositor->refusedCount();
        client.start(m_compositor->socketName(), presented);
        m_compositor->expect(QStringLiteral("fixture"), expected, client.pid);
        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());

        QTRY_VERIFY_WITH_TIMEOUT(m_compositor->refusedCount() > before, 10000);
        QCOMPARE(refused.count(), 1);   // the toplevel, once; its pid passed the gate
        QCOMPARE(refused.last().at(0).toLongLong(), client.pid);
        QCOMPARE(refused.last().at(1).toString(), QStringLiteral("logos-view-") + presented);
        // Closed by the compositor: the client's connection drops and Qt's
        // wayland client exits.
        QTRY_VERIFY_WITH_TIMEOUT(client.process.state() == QProcess::NotRunning, 10000);
        QVERIFY(!item->isReady());
        QVERIFY(!m_compositor->boundSurface(expected));
        m_compositor->forget(expected);
        client.stop();
    }

    // Admission: a pid nobody expects is closed on its first surface request,
    // and the rest of that first batch (its toplevel) is not counted again.
    void unknownPidIsRefused()
    {
        Client client;
        QSignalSpy refused(m_compositor, &SurfaceCompositor::surfaceRefused);
        const int before = m_compositor->refusedCount();
        client.start(m_compositor->socketName(), newToken());
        QTRY_VERIFY_WITH_TIMEOUT(m_compositor->refusedCount() > before, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(client.process.state() == QProcess::NotRunning, 10000);
        QCOMPARE(refused.count(), 1);
        QCOMPARE(refused.first().at(0).toLongLong(), client.pid);
        QVERIFY(refused.first().at(1).toString().isEmpty());   // pid gate, no app_id
        client.stop();
    }

    // Sandbox invariants (RFC-3): a view may only name its own module, and a
    // module nobody spawned as gui has no token.
    void itemRefusesOtherModules()
    {
        {
            // Engine bound as "evil", QML asks for "fixture".
            QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("evil")));
            LogosSurfaceItem* item = surfaceOf(host.data());
            QTRY_COMPARE_WITH_TIMEOUT(item->property("errors").toList(),
                                      QVariantList{QStringLiteral("tokenMismatch")}, 2000);
            QCOMPARE(item->transport(), QStringLiteral("none"));
        }
        {
            // Right module, but no expectation for it.
            QVERIFY(m_compositor->tokenFor(QStringLiteral("fixture")).isEmpty());
            QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
            LogosSurfaceItem* item = surfaceOf(host.data());
            QTRY_COMPARE_WITH_TIMEOUT(item->property("errors").toList(),
                                      QVariantList{QStringLiteral("notGuiModule")}, 2000);
        }
    }

    // RFC-4 rev 2 H2: the registry shows one toplevel's worth of protocol
    // and nothing else, and a hidden global cannot be bound by guessing.
    void rawClientSeesOnlyAllowedGlobals()
    {
#ifndef LOGOS_HAVE_RAW_WAYLAND_CLIENT
        QSKIP("raw libwayland client not built (wayland-scanner/wayland-protocols missing)");
#else
        QVERIFY(!m_compositor->gpuBuffersAllowed());
        QVERIFY(SurfaceCompositor::isGlobalAllowed("wl_shm", false));
        QVERIFY(!SurfaceCompositor::isGlobalAllowed("zwp_linux_dmabuf_v1", false));
        QVERIFY(SurfaceCompositor::isGlobalAllowed("zwp_linux_dmabuf_v1", true));
        QVERIFY(!SurfaceCompositor::isGlobalAllowed("wl_data_device_manager", true));

        RawClient client;
        const QString token = newToken();
        client.start(m_compositor->socketName(), token, QStringLiteral("probe"));
        m_compositor->expect(QStringLiteral("fixture"), token, client.pid);
        QTRY_VERIFY_WITH_TIMEOUT(client.finished(), 15000);

        const QStringList globals = client.globals();
        QVERIFY2(!globals.isEmpty(), "no registry listing");
        for (const char* needed : {"wl_compositor", "wl_subcompositor", "wl_shm", "wl_seat",
                                   "wl_output", "xdg_wm_base"})
            QVERIFY2(globals.contains(QLatin1String(needed)), needed);
        for (const QString& g : globals) {
            QVERIFY2(SurfaceCompositor::isGlobalAllowed(qPrintable(g), false), qPrintable(g));
            QVERIFY2(!g.startsWith(QLatin1String("qt_")), qPrintable(g));
        }
        QVERIFY(!globals.contains(QStringLiteral("zwp_linux_dmabuf_v1")));
        QVERIFY(!globals.contains(QStringLiteral("wl_data_device_manager")));
        // The bind attempt ended in a protocol error, never in a silent grant.
        QVERIFY2(client.has(QStringLiteral("BIND_ERROR")), qPrintable(client.lines.join(QLatin1Char('|'))));
        m_compositor->forget(token);
#endif
    }

    // RFC-4 rev 2 H3: a buffer over the bound closes the client before the
    // host shows it; the item reports the loss.
    void rawOversizeBufferClosesClient()
    {
#ifndef LOGOS_HAVE_RAW_WAYLAND_CLIENT
        QSKIP("raw libwayland client not built");
#else
        RawClient client;
        const QString token = newToken();
        QSignalSpy refused(m_compositor, &SurfaceCompositor::surfaceRefused);
        QSignalSpy lost(m_compositor, &SurfaceCompositor::surfaceLost);
        client.start(m_compositor->socketName(), token, QStringLiteral("oversize"));
        m_compositor->expect(QStringLiteral("fixture"), token, client.pid);
        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());

        QTRY_VERIFY_WITH_TIMEOUT(client.finished(), 20000);
        QVERIFY2(client.has(QStringLiteral("DISCONNECTED")), qPrintable(client.lines.join(QLatin1Char('|'))));
        QTRY_COMPARE_WITH_TIMEOUT(refused.count(), 1, 5000);
        QCOMPARE(refused.first().at(0).toLongLong(), client.pid);
        QCOMPARE(refused.first().at(1).toString(), QStringLiteral("oversize"));
        QCOMPARE(lost.count(), 1);
        QVERIFY(!m_compositor->boundSurface(token));
        QTRY_VERIFY_WITH_TIMEOUT(!item->isReady(), 5000);
        QVERIFY(item->property("errors").toList().contains(QStringLiteral("clientExited")));
        m_compositor->forget(token);
#endif
    }

    // RFC-4 rev 2 H1: a pool truncated behind a committed buffer costs the
    // client its connection and the host nothing. Needs the guarded-access
    // qtwayland (nix/patches); on stock Qt this test takes the host down.
    void rawTruncatedPoolDisconnectsOnlyTheClient()
    {
#ifndef LOGOS_HAVE_RAW_WAYLAND_CLIENT
        QSKIP("raw libwayland client not built");
#else
        RawClient client;
        const QString token = newToken();
        QSignalSpy lost(m_compositor, &SurfaceCompositor::surfaceLost);
        client.start(m_compositor->socketName(), token, QStringLiteral("truncate"));
        m_compositor->expect(QStringLiteral("fixture"), token, client.pid);
        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());

        QTRY_VERIFY_WITH_TIMEOUT(item->isReady(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(client.has(QStringLiteral("TRUNCATED")), 15000);
        // The host keeps rendering the item; the next read of the pool
        // faults inside the guard and the client is dropped.
        QTRY_VERIFY2_WITH_TIMEOUT(client.finished(),
                                  qPrintable(client.lines.join(QLatin1Char('|'))), 20000);
        QVERIFY2(client.has(QStringLiteral("DISCONNECTED")), qPrintable(client.lines.join(QLatin1Char('|'))));
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!item->isReady(), 5000);
        m_compositor->forget(token);

        // And the host is fine: a well-behaved client binds afterwards.
        Client good;
        const QString token2 = newToken();
        good.start(m_compositor->socketName(), token2);
        m_compositor->expect(QStringLiteral("fixture"), token2, good.pid);
        QScopedPointer<QQuickWidget> host2(makeHost(QStringLiteral("fixture")));
        QTRY_VERIFY_WITH_TIMEOUT(surfaceOf(host2.data())->isReady(), 10000);
        m_compositor->forget(token2);
        good.stop();
#endif
    }

    // The host-only channel: nothing about tokens or module identity is
    // reachable from QML on the item.
    void itemExposesNoTokenToQml()
    {
        const QMetaObject* mo = &LogosSurfaceItem::staticMetaObject;
        QCOMPARE(mo->indexOfProperty("token"), -1);
        for (int i = 0; i < mo->methodCount(); ++i) {
            const QString name = QString::fromLatin1(mo->method(i).name());
            QVERIFY2(!name.contains(QStringLiteral("token"), Qt::CaseInsensitive)
                     && !name.contains(QStringLiteral("bindEngine")),
                     qPrintable(name));
        }
    }
};

int main(int argc, char** argv)
{
    if (argc >= 3 && qstrcmp(argv[1], "--surface-client") == 0)
        return runClient(argc, argv);
#ifdef LOGOS_HAVE_RAW_WAYLAND_CLIENT
    if (argc >= 4 && qstrcmp(argv[1], "--raw-client") == 0)
        return runRawClient(argc, argv);
#endif

    // Host: offscreen at DPR 2 with the software scene graph (no GPU in CI).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_SCALE_FACTOR", "2");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QApplication app(argc, argv);
    ViewSurfaceTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "view_surface_test.moc"
