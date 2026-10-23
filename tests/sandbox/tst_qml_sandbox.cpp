// Regression test for F-008: a "sandboxed" ui_qml module loading arbitrary
// native code via a QML qmldir `plugin` directive (escaping the QML sandbox).
//
// The exploit: a ui_qml package ships, under its install dir, a malicious QML
// module whose qmldir declares a native `plugin`, plus the matching Qt plugin
// .so. When basecamp loads the module's view.qml, the engine resolves the
// `import Evil`, finds the qmldir, resolves the plugin, and dlopen()s the .so
// INTO THE HOST PROCESS — running attacker C++ with full host privileges,
// defeating the network-deny + filesystem-restriction guarantees the ui_qml
// sandbox is supposed to provide.
//
// This test stands up exactly that attacker layout and drives the REAL
// production sandbox setup (QmlSandbox::configure — the same code path
// PluginLoader::loadQmlView uses). The "evil" plugin's constructor writes a
// sentinel file when it is loaded; the test asserts the sentinel is NEVER
// created. Two attack vectors are covered:
//   1. qmldir `plugin evil_plugin`            (relative — resolved via plugin path)
//   2. qmldir `plugin evil_plugin <abs path>` (absolute — prepended to search,
//                                               bypasses pluginPathList entirely)
// Both must be blocked. A third case proves a legitimate pure-QML module still
// loads, so the fix does not over-restrict.
//
// To prove the test is meaningful, run with SANDBOX_TEST_EXPECT_ESCAPE=1: it
// then loads the malicious module WITHOUT the sandbox and asserts the sentinel
// DOES appear, confirming the plugin really is capable of executing.
//
// Beyond F-008, this file also guards the OTHER ui_qml sandbox guarantees —
// the ones the counter_qml probe app (repos/counter_qml/Main.qml) checks by
// hand: network deny (HTTP and file:// via XMLHttpRequest), URL-interceptor
// blocking of remote-scheme loads and out-of-sandbox file reads (Loader/Image/
// Qt.openUrlExternally), and the matching positive cases (a file under the
// module's own dir, and qrc: resources, still resolve). These guarantees used
// to live inline in PluginLoader and were refactored into QmlSandbox; the
// sandbox* slots below drive that same production config so a regression in any
// of them fails here. See the block comment above those slots for the mapping.

#include <QtTest/QtTest>

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickTextureFactory>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <memory>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>

#include "restricted/QmlSandbox.h"
#include "restricted/SandboxImages.h"

namespace {

// Absolute path to the prebuilt evil Qt plugin (.so/.dylib). Provided by CMake
// via -DEVIL_PLUGIN_PATH, with an env override for flexibility.
QString evilPluginPath()
{
    if (const QByteArray fromEnv = qgetenv("EVIL_PLUGIN_PATH"); !fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);
#ifdef EVIL_PLUGIN_PATH
    return QStringLiteral(EVIL_PLUGIN_PATH);
#else
    return QString();
#endif
}

void writeFile(const QString& path, const QString& contents)
{
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Text),
             qPrintable(QStringLiteral("cannot write ") + path));
    QTextStream(&f) << contents;
}

// Source dir of the evil_app fixture (tests/sandbox/evil_app). Provided by CMake
// via -DEVIL_APP_DIR, with an env override mirroring evilPluginPath().
QString evilAppDir()
{
    if (const QByteArray fromEnv = qgetenv("EVIL_APP_DIR"); !fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);
#ifdef EVIL_APP_DIR
    return QStringLiteral(EVIL_APP_DIR);
#else
    return QString();
#endif
}

// Recursively copy a directory tree (used to stage the evil_app fixture into a
// writable QTemporaryDir so it becomes a real, canonical sandbox install root).
bool copyTree(const QString& srcDir, const QString& dstDir)
{
    QDir().mkpath(dstDir);
    QDir src(srcDir);
    for (const QFileInfo& entry :
         src.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dst = dstDir + "/" + entry.fileName();
        if (entry.isDir()) {
            if (!copyTree(entry.absoluteFilePath(), dst))
                return false;
        } else {
            QFile::remove(dst);
            if (!QFile::copy(entry.absoluteFilePath(), dst))
                return false;
        }
    }
    return true;
}

// Base directory for the test's scratch dirs. The attacker plugin gets dlopen()ed
// from here, so it must be on an exec-capable filesystem — the default temp dir
// (/tmp) is mounted noexec in some sandboxes, which would make dlopen fail for an
// unrelated reason. SANDBOX_TEST_TMPDIR (set by CMake to the build tree) overrides.
QString workRoot()
{
    if (const QByteArray fromEnv = qgetenv("SANDBOX_TEST_TMPDIR"); !fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);
    return QDir::tempPath();
}

// ---- Helpers for the in-memory image slots --------------------------------

// A picture whose content differs with `seed`, so each one gives a new data URL.
QImage testPicture(int w, int h, int seed)
{
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixel(x, y, qRgb((x * 7 + seed * 31) & 255, (y * 5 + seed * 17) & 255,
                                    (x + y + seed * 53) & 255));
    return img;
}

// Blue, with a red block in the top-left corner: shows which way up it is.
QImage markedPicture(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(qRgb(0, 0, 255));
    for (int y = 0; y < h / 3; ++y)
        for (int x = 0; x < w / 3; ++x)
            img.setPixel(x, y, qRgb(255, 0, 0));
    return img;
}

bool isRed(QRgb c)  { return qRed(c) > 180 && qBlue(c) < 90; }
bool isBlue(QRgb c) { return qBlue(c) > 180 && qRed(c) < 90; }

QByteArray encodeImage(const QImage& img, const char* format)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, format);
    return bytes;
}

QString dataUrlOf(const QByteArray& bytes, const char* mime)
{
    return QStringLiteral("data:%1;base64,").arg(QLatin1String(mime))
         + QString::fromLatin1(bytes.toBase64());
}

// The compressed bytes sourceFor() charges for a data URL.
qint64 costOf(const QString& dataUrl)
{
    return qint64(dataUrl.size() - dataUrl.indexOf(QLatin1Char(',')) - 1) * 3 / 4;
}

// Inserts an EXIF "Orientation" tag right after the JPEG start marker, as a phone does.
QByteArray withExifOrientation(const QByteArray& jpeg, quint16 orientation)
{
    QByteArray tiff;
    tiff += "MM";
    tiff += QByteArray::fromHex("002A00000008");  // big-endian, magic, IFD at byte 8
    tiff += QByteArray::fromHex("0001");          // one entry
    tiff += QByteArray::fromHex("011200030000000" "1");  // Orientation, SHORT, count 1
    tiff += char(orientation >> 8);
    tiff += char(orientation & 0xFF);
    tiff += QByteArray(2, '\0');                  // value padding
    tiff += QByteArray(4, '\0');                  // no next IFD
    const QByteArray payload = QByteArray("Exif\0\0", 6) + tiff;
    const quint16 length = quint16(payload.size() + 2);
    QByteArray segment;
    segment += char(0xFF);
    segment += char(0xE1);
    segment += char(length >> 8);
    segment += char(length & 0xFF);
    segment += payload;
    return jpeg.left(2) + segment + jpeg.mid(2);
}

// Loads one image through the engine's provider the way an Image item does, and
// waits for it. Returns a null image (and the reason) when it cannot be shown.
QImage loadImage(QQmlEngine& engine, const QString& url, const QSize& requested = QSize(),
                 QString* error = nullptr)
{
    auto* provider = dynamic_cast<QQuickAsyncImageProvider*>(engine.imageProvider(QStringLiteral("sandbox-media")));
    if (!provider) return {};
    std::unique_ptr<QQuickImageResponse> response(
        provider->requestImageResponse(QUrl(url).path().mid(1), requested));
    QSignalSpy spy(response.get(), &QQuickImageResponse::finished);
    if (spy.isEmpty() && !spy.wait(10000)) return {};
    if (error) *error = response->errorString();
    std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
    return factory ? factory->image() : QImage();
}

SandboxImages* imagesOf(QQmlEngine& engine)
{
    return qobject_cast<SandboxImages*>(
        engine.rootContext()->contextProperty(QStringLiteral("SandboxImages")).value<QObject*>());
}

} // namespace

class QmlSandboxTest : public QObject
{
    Q_OBJECT

private:
    QString m_evilPlugin;

    // Builds an attacker install dir under `root` for a malicious QML module
    // named `moduleName`:
    //   <root>/view.qml               imports the malicious module
    //   <root>/<moduleName>/qmldir    declares the native plugin
    //   <root>/<moduleName>/<plugin>  the real evil Qt plugin (copied in)
    // When `absolutePluginPath` is true the qmldir uses the absolute-path form.
    // Writes the view.qml path into `viewOut`. (Uses QVERIFY2, which returns
    // void on failure, so it must be a void helper.)
    //
    // The module name MUST be unique per test: Qt records loaded plugins by
    // module URI in a PROCESS-WIDE cache (modulesForWhichPluginsHaveBeenLoaded),
    // so reusing a name would let one test's load short-circuit another's and
    // mask an escape. Tests run in separate processes too (see CMakeLists), but
    // the unique name keeps single-process dev runs honest as well.
    void stageMaliciousModule(const QString& root, const QString& moduleName,
                              bool absolutePluginPath, QString& viewOut)
    {
        const QString modDir = root + "/" + moduleName;
        QDir().mkpath(modDir);

        const QString pluginFile = QFileInfo(m_evilPlugin).fileName();
        QVERIFY2(QFile::copy(m_evilPlugin, modDir + "/" + pluginFile),
                 "failed to stage evil plugin");

        // Qt strips the lib prefix / suffix to get the plugin base name.
        QString base = QFileInfo(pluginFile).completeBaseName();
        if (base.startsWith(QLatin1String("lib")))
            base = base.mid(3);

        QString qmldir = QStringLiteral("module %1\n").arg(moduleName);
        if (absolutePluginPath)
            qmldir += QStringLiteral("plugin %1 %2\n").arg(base, modDir);
        else
            qmldir += QStringLiteral("plugin %1\n").arg(base);
        writeFile(modDir + "/qmldir", qmldir);

        // Import QtQml (not QtQuick): its root type QtObject is a builtin
        // compiled into libQt6Qml and needs no native plugin of its own, so the
        // view depends on nothing but the engine itself. This keeps the test
        // hermetic — the ONLY native plugin in play is the attacker's one, so a
        // created sentinel can only mean the escape fired.
        const QString view = root + "/view.qml";
        writeFile(view, QStringLiteral("import QtQml\nimport %1\nQtObject {}\n").arg(moduleName));
        viewOut = view;
    }

    // Loads `viewPath` through a QML engine, optionally applying the production
    // sandbox. Returns true if the evil plugin's sentinel was created (i.e. the
    // escape succeeded).
    bool attemptLoad(const QString& viewPath, const QString& installDir, bool applySandbox)
    {
        QTemporaryDir sentinelDir(workRoot() + QStringLiteral("/tst_qml_sandbox-sentinel-XXXXXX"));
        const QString sentinel = sentinelDir.path() + "/PWNED";
        qputenv("EVIL_PLUGIN_SENTINEL", sentinel.toLocal8Bit());
        QFile::remove(sentinel);

        {
            QQmlEngine engine;
            if (applySandbox)
                QmlSandbox::configure(&engine, installDir, viewPath, /*appLibDir=*/QString());
            QQmlComponent component(&engine, QUrl::fromLocalFile(viewPath));
            // create() forces type compilation/instantiation; harmless if errored.
            QScopedPointer<QObject> obj(component.create());
            Q_UNUSED(obj);
        }
        return QFile::exists(sentinel);
    }

private slots:
    void initTestCase()
    {
        m_evilPlugin = evilPluginPath();
        QVERIFY2(!m_evilPlugin.isEmpty(),
                 "EVIL_PLUGIN_PATH not set (build wires it via -DEVIL_PLUGIN_PATH)");
        QVERIFY2(QFile::exists(m_evilPlugin),
                 qPrintable(QStringLiteral("evil plugin missing: ") + m_evilPlugin));
    }

    // Sanity: without the sandbox the attack actually works, so the negative
    // tests below are meaningful and not passing for some unrelated reason
    // (e.g. the plugin being silently non-loadable in this environment).
    void evilPluginReallyExecutes_whenUnsandboxed()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());
        QString view;
        stageMaliciousModule(dir.path(), QStringLiteral("EvilUnsandboxed"),
                             /*absolutePluginPath=*/false, view);

        // Mirror the OLD vulnerable engine config: module dir on import + plugin paths.
        QTemporaryDir sentinelDir(workRoot() + QStringLiteral("/tst_qml_sandbox-sentinel-XXXXXX"));
        const QString sentinel = sentinelDir.path() + "/PWNED";
        qputenv("EVIL_PLUGIN_SENTINEL", sentinel.toLocal8Bit());
        QFile::remove(sentinel);
        {
            QQmlEngine engine;
            QStringList importPaths = engine.importPathList();
            importPaths.prepend(dir.path());
            engine.setImportPathList(importPaths);
            QStringList pluginPaths = engine.pluginPathList();
            pluginPaths.prepend(dir.path());
            engine.setPluginPathList(pluginPaths);
            QQmlComponent component(&engine, QUrl::fromLocalFile(view));
            QScopedPointer<QObject> obj(component.create());
            Q_UNUSED(obj);
            // If the precondition ever regresses (e.g. a noexec temp dir defeats
            // dlopen, or a Qt change), surface why rather than just "sentinel
            // missing" — this is the one place the malicious load is expected to
            // succeed, so its errors are diagnostic, not failures.
            if (component.isError())
                qWarning().noquote() << "unsandboxed load errors:" << component.errorString();
        }
        QVERIFY2(QFile::exists(sentinel),
                 "precondition failed: the evil plugin did not load even WITHOUT the "
                 "sandbox — the negative tests would be vacuous");
    }

    // The fix: the relative-plugin-path vector must be blocked.
    void sandboxBlocksNativePlugin_relativePath()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());
        QString view;
        stageMaliciousModule(dir.path(), QStringLiteral("EvilRelative"),
                             /*absolutePluginPath=*/false, view);
        QVERIFY2(!attemptLoad(view, dir.path(), /*applySandbox=*/true),
                 "SANDBOX ESCAPE: native plugin executed via relative qmldir plugin path");
    }

    // The fix: the absolute-plugin-path vector must ALSO be blocked. (This is the
    // vector that survives a naive "remove installDir from pluginPathList" fix,
    // because Qt prepends an absolute qmldir plugin path to the search regardless.)
    void sandboxBlocksNativePlugin_absolutePath()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());
        QString view;
        stageMaliciousModule(dir.path(), QStringLiteral("EvilAbsolute"),
                             /*absolutePluginPath=*/true, view);
        QVERIFY2(!attemptLoad(view, dir.path(), /*applySandbox=*/true),
                 "SANDBOX ESCAPE: native plugin executed via absolute qmldir plugin path");
    }

    // The fix must not over-restrict: a legitimate pure-QML module (no native
    // plugin directive) shipped by the ui_qml module must still load.
    void sandboxAllowsPureQmlModule()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());
        // Stage a pure-QML module + a view that imports it. Everything is built
        // on QtQml's builtin QtObject so the positive case is hermetic (no
        // dependency on QtQuick's native plugin being resolvable in the test env).
        const QString modDir = dir.path() + "/LegitWidgets";
        QDir().mkpath(modDir);
        writeFile(modDir + "/qmldir",
                  QStringLiteral("module LegitWidgets\nGadget 1.0 Gadget.qml\n"));
        writeFile(modDir + "/Gadget.qml",
                  QStringLiteral("import QtQml\nQtObject { property int size: 5 }\n"));
        const QString view = dir.path() + "/view.qml";
        writeFile(view, QStringLiteral(
                            "import QtQml\nimport LegitWidgets\n"
                            "QtObject { property Gadget g: Gadget {} }\n"));

        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), view, /*appLibDir=*/QString());
        QQmlComponent component(&engine, QUrl::fromLocalFile(view));
        QScopedPointer<QObject> obj(component.create());
        QVERIFY2(!component.isError(),
                 qPrintable(QStringLiteral("legit pure-QML module failed to load: ")
                            + component.errorString()));
        QVERIFY2(!obj.isNull(), "legit pure-QML module produced no object");
    }

    // ----------------------------------------------------------------------
    // The remaining slots cover the other sandbox guarantees that the
    // counter_qml probe app (repos/counter_qml/Main.qml) exercises by hand:
    // network deny (HTTP + file via XMLHttpRequest), URL-interceptor blocking
    // of remote/foreign-scheme loads (Loader/Image) and out-of-sandbox file
    // reads, and Qt.openUrlExternally. They all drive the SAME production
    // sandbox config (QmlSandbox::configure) so a regression in any of these
    // older guarantees — easy to introduce now that the policy was refactored
    // out of PluginLoader — fails here instead of silently shipping.

    // Network deny: QmlSandbox must install a QQmlNetworkAccessManagerFactory
    // whose NAM fails every request, so any HTTP/HTTPS the QML attempts (e.g.
    // counter_qml's "HTTP GET https://example.com" probe, which uses
    // XMLHttpRequest -> the engine NAM) cannot reach the network.
    void sandboxDeniesNetworkAccess()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());

        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml",
                              /*appLibDir=*/QString());

        // The engine's NAM is the one XMLHttpRequest uses under the hood.
        QNetworkAccessManager* nam = engine.networkAccessManager();
        QVERIFY2(nam, "sandbox left the engine without a network access manager");

        QNetworkRequest req(QUrl(QStringLiteral("https://example.com/")));
        QScopedPointer<QNetworkReply> reply(nam->get(req));
        QVERIFY2(!reply.isNull(), "deny-all NAM returned no reply object");

        QSignalSpy finished(reply.data(), &QNetworkReply::finished);
        QVERIFY2(finished.wait(5000), "deny-all reply never finished");
        QVERIFY2(reply->error() != QNetworkReply::NoError,
                 "SANDBOX BREACH: network request was permitted (no error)");
    }

    // Same guarantee for the file:// scheme: counter_qml probes
    // "Read file:///etc/hosts" via XMLHttpRequest. The deny-all NAM must fail
    // that too — the QML engine must not become a file-read primitive.
    void sandboxDeniesFileSchemeNetworkRequest()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());

        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml",
                              /*appLibDir=*/QString());
        QNetworkAccessManager* nam = engine.networkAccessManager();
        QVERIFY2(nam, "sandbox left the engine without a network access manager");

        QNetworkRequest req(QUrl(QStringLiteral("file:///etc/hosts")));
        QScopedPointer<QNetworkReply> reply(nam->get(req));
        QVERIFY2(!reply.isNull(), "deny-all NAM returned no reply object");

        QSignalSpy finished(reply.data(), &QNetworkReply::finished);
        QVERIFY2(finished.wait(5000), "deny-all reply never finished");
        QVERIFY2(reply->error() != QNetworkReply::NoError,
                 "SANDBOX BREACH: file:// request was permitted (no error)");
    }

    // URL interception: a non-qrc, non-local scheme (http/https) must be
    // blocked outright. This is the choke point behind counter_qml's
    // "Loader source from http://example.com/fake.qml" probe — the engine asks
    // the interceptor to resolve the remote URL and must get an empty QUrl back.
    void sandboxBlocksRemoteSchemeUrl()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());

        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml",
                              /*appLibDir=*/QString());

        const QUrl remote(QStringLiteral("http://example.com/fake.qml"));
        // interceptUrl runs every interceptor the engine has; an empty result
        // means "blocked" (the resource will not resolve / load).
        const QUrl out =
            engine.interceptUrl(remote, QQmlAbstractUrlInterceptor::UrlString);
        QVERIFY2(out.isEmpty(),
                 qPrintable(QStringLiteral("SANDBOX BREACH: remote URL was allowed to resolve to ")
                            + out.toString()));
    }

    // URL interception: a local file OUTSIDE the sandbox roots must be blocked.
    // Backs counter_qml's "Image source file:///etc/hosts" and
    // "openUrlExternally(file:///etc/hosts)" probes — both funnel the foreign
    // path through the engine's interceptor (Image via the QML image pipeline,
    // Qt.openUrlExternally via QtObject::resolvedUrl -> interceptUrl(UrlString)).
    void sandboxBlocksFileOutsideRoots()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());

        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml",
                              /*appLibDir=*/QString());

        const QUrl outside = QUrl::fromLocalFile(QStringLiteral("/etc/hosts"));
        for (auto type : {QQmlAbstractUrlInterceptor::UrlString,
                          QQmlAbstractUrlInterceptor::QmlFile}) {
            const QUrl out = engine.interceptUrl(outside, type);
            QVERIFY2(out.isEmpty(),
                     qPrintable(QStringLiteral("SANDBOX BREACH: file outside roots resolved to ")
                                + out.toString()));
        }
    }

    // The interceptor must not over-restrict: a local file UNDER the module's
    // own install dir is the legitimate case (its own QML/JS/assets) and must
    // resolve unchanged. Pairs with sandboxBlocksFileOutsideRoots so a fix that
    // simply blocked all local files would fail here.
    void sandboxAllowsFileUnderInstallDir()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());
        // The path must exist: the interceptor canonicalises and compares real
        // paths, and a non-existent path canonicalises to empty.
        writeFile(dir.path() + "/asset.txt", QStringLiteral("hello\n"));

        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml",
                              /*appLibDir=*/QString());

        const QUrl inside = QUrl::fromLocalFile(dir.path() + "/asset.txt");
        const QUrl out =
            engine.interceptUrl(inside, QQmlAbstractUrlInterceptor::UrlString);
        QVERIFY2(!out.isEmpty(),
                 "sandbox over-restricted: a file under the module's own install dir was blocked");
    }

    // BUG-018 (revised): a NON-EXISTENT local path must PASS THROUGH, not be
    // blocked. Qt's module resolution probes many non-existent
    // "<importPath>/<Module>[.ver]/qmldir" candidates before finding the real
    // one; blocking a miss (returning an invalid URL) aborts resolution and
    // breaks legitimate imports like a ui_qml view's `import Panels`. A path
    // that does not exist can't load anything — if it later resolves to a real
    // file, that file is re-intercepted with a non-empty canonical path and
    // vetted against the roots then. (An earlier fix blocked these outright and
    // regressed every sandboxed view's imports — see RestrictedUrlInterceptor.)
    void sandboxPassesThroughNonExistentProbe()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());

        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml",
                              /*appLibDir=*/QString());

        // Inside the install dir but does not exist -> canonicalPath is empty.
        // Must pass through unchanged so Qt can continue probing.
        const QUrl ghost =
            QUrl::fromLocalFile(dir.path() + "/does_not_exist/ghost.qml");
        for (auto type : {QQmlAbstractUrlInterceptor::UrlString,
                          QQmlAbstractUrlInterceptor::QmlFile}) {
            const QUrl out = engine.interceptUrl(ghost, type);
            QVERIFY2(out == ghost,
                     qPrintable(QStringLiteral("non-existent probe candidate was not "
                                               "passed through; got ") + out.toString()));
        }
    }

    // qrc: resources are engine builtins (the app's own bundled QML), never the
    // untrusted module's, and must always pass the interceptor untouched.
    void sandboxAllowsQrcScheme()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());

        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml",
                              /*appLibDir=*/QString());

        const QUrl qrc(QStringLiteral("qrc:/qt/qml/Foo/Bar.qml"));
        const QUrl out =
            engine.interceptUrl(qrc, QQmlAbstractUrlInterceptor::QmlFile);
        QCOMPARE(out, qrc);
    }

    // ----------------------------------------------------------------------
    // In-memory images: the scoped provider that lets a sandboxed plugin show
    // pictures it was handed as data: URLs, without opening data: to the sandbox.
    // ----------------------------------------------------------------------

    void sandboxSupportsScopedInMemoryImages()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_sandbox_images-XXXXXX"));
        QVERIFY(dir.isValid());
        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml", QString());
        SandboxImages* images = imagesOf(engine);
        QVERIFY(images);

        const QImage input(2, 2, QImage::Format_RGB32);
        QImage red = input;
        red.fill(Qt::red);
        const QString dataUrl = dataUrlOf(encodeImage(red, "PNG"), "image/png");
        const QString url = images->sourceFor(dataUrl);
        QVERIFY(QRegularExpression(QStringLiteral("^image://sandbox-media/[0-9a-f]{24}$")).match(url).hasMatch());
        QCOMPARE(images->sourceFor(dataUrl), url);
        QCOMPARE(engine.interceptUrl(QUrl(url), QQmlAbstractUrlInterceptor::UrlString), QUrl(url));
        QCOMPARE(engine.interceptUrl(QUrl(url), QQmlAbstractUrlInterceptor::QmlFile), QUrl());
        QCOMPARE(engine.interceptUrl(QUrl(QStringLiteral("image://other/123")),
                                     QQmlAbstractUrlInterceptor::UrlString), QUrl());
        QCOMPARE(engine.interceptUrl(QUrl(dataUrl), QQmlAbstractUrlInterceptor::UrlString), QUrl());
        QVERIFY(images->sourceFor(QStringLiteral("data:text/html;base64,PHNjcmlwdD4=")).isEmpty());
        QVERIFY(images->sourceFor(QStringLiteral("data:image/svg+xml;base64,PHN2Zy8+")).isEmpty());
        QVERIFY(images->sourceFor(QStringLiteral("file:///etc/hosts")).isEmpty());
        QVERIFY(images->sourceFor(QString()).isEmpty());

        const QImage resolved = loadImage(engine, url);
        QVERIFY(!resolved.isNull());
        QCOMPARE(resolved.size(), QSize(2, 2));
        QCOMPARE(resolved.pixelColor(0, 0), QColor(Qt::red));

        // Another QML engine must not have access to this engine's images.
        QQmlEngine second;
        QmlSandbox::configure(&second, dir.path(), dir.path() + "/view.qml", QString());
        QString why;
        QVERIFY(loadImage(second, url, QSize(), &why).isNull());
        QVERIFY(!why.isEmpty());
    }

    // A request is never scaled UP, and never beyond the largest allowed image:
    // asking for Qt.size(30000, 30000) used to allocate gigabytes.
    void sandboxClampsRequestedSize()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_sandbox_images-XXXXXX"));
        QVERIFY(dir.isValid());
        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml", QString());
        SandboxImages* images = imagesOf(engine);
        QVERIFY(images);

        const QString tiny = images->sourceFor(dataUrlOf(encodeImage(testPicture(2, 2, 1), "PNG"), "image/png"));
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(loadImage(engine, tiny, QSize(30000, 30000)).size(), QSize(2, 2));
        QCOMPARE(loadImage(engine, tiny, QSize(30000, 0)).size(), QSize(2, 2));
        QCOMPARE(loadImage(engine, tiny, QSize(8000, 8000)).size(), QSize(2, 2));
        QVERIFY2(timer.elapsed() < 5000, "oversized requests must stay cheap");

        const QString wide = images->sourceFor(dataUrlOf(encodeImage(testPicture(64, 32, 2), "PNG"), "image/png"));
        QCOMPARE(loadImage(engine, wide).size(), QSize(64, 32));            // no preference
        QCOMPARE(loadImage(engine, wide, QSize(-1, -1)).size(), QSize(64, 32));
        QCOMPARE(loadImage(engine, wide, QSize(16, 16)).size(), QSize(16, 8));   // shrinks, keeps the ratio
        QCOMPARE(loadImage(engine, wide, QSize(1000, 16)).size(), QSize(32, 16)); // only the limit that bites
        QCOMPARE(loadImage(engine, wide, QSize(20, 0)).size(), QSize(20, 10));   // one side given
    }

    // Decoding happens at the size shown, and a phone photo that is stored on
    // its side (EXIF orientation) is shown upright.
    void sandboxDecodesAtDisplaySizeAndTurnsPhotosUpright()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_sandbox_images-XXXXXX"));
        QVERIFY(dir.isValid());
        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml", QString());
        SandboxImages* images = imagesOf(engine);
        QVERIFY(images);

        const QByteArray plain = encodeImage(testPicture(400, 300, 3), "JPEG");
        QVERIFY(!plain.isEmpty());
        const QString big = images->sourceFor(dataUrlOf(plain, "image/jpeg"));
        QCOMPARE(loadImage(engine, big).size(), QSize(400, 300));
        QCOMPARE(loadImage(engine, big, QSize(100, 100)).size(), QSize(100, 75));

        // Orientation 6 means "turn it a quarter turn clockwise to view it": stored
        // as 100x50 with the red block top-left, shown as 50x100 with it top-right.
        const QByteArray sideways = withExifOrientation(encodeImage(markedPicture(100, 50), "JPEG"), 6);
        const QString turned = images->sourceFor(dataUrlOf(sideways, "image/jpeg"));
        QVERIFY(!turned.isEmpty());
        const QImage upright6 = loadImage(engine, turned);
        QCOMPARE(upright6.size(), QSize(50, 100));
        QVERIFY2(isRed(upright6.pixel(44, 10)), "the red block should be top-right");
        QVERIFY2(isBlue(upright6.pixel(5, 10)), "top-left should be blue");
        QVERIFY2(isBlue(upright6.pixel(25, 80)), "the rest should be blue");
        const QImage small = loadImage(engine, turned, QSize(25, 25));
        QCOMPARE(small.height(), 25);
        QVERIFY2(qAbs(small.width() - 13) <= 1, qPrintable(QStringLiteral("width %1").arg(small.width())));
        QVERIFY2(isRed(small.pixel(small.width() - 3, 3)), "still top-right when scaled down");
        QVERIFY2(isBlue(small.pixel(2, 3)), "still blue top-left when scaled down");

        // Orientation 1 (normal) leaves it alone.
        const QByteArray upright = withExifOrientation(encodeImage(markedPicture(100, 50), "JPEG"), 1);
        const QImage normal = loadImage(engine, images->sourceFor(dataUrlOf(upright, "image/jpeg")));
        QCOMPARE(normal.size(), QSize(100, 50));
        QVERIFY(isRed(normal.pixel(10, 5)));
        QVERIFY(isBlue(normal.pixel(80, 40)));
    }

    // Per-app and all-apps budgets: over budget, sourceFor() returns nothing and
    // keeps nothing; release() gives the room back.
    void sandboxEnforcesBudgetsAndRelease()
    {
        const QString a = dataUrlOf(encodeImage(testPicture(32, 32, 11), "PNG"), "image/png");
        const QString b = dataUrlOf(encodeImage(testPicture(32, 32, 12), "PNG"), "image/png");
        const QString c = dataUrlOf(encodeImage(testPicture(32, 32, 13), "PNG"), "image/png");
        SandboxImages::limits().appCompressedBytes = costOf(a) + costOf(b) + costOf(c) / 2;

        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_sandbox_images-XXXXXX"));
        QVERIFY(dir.isValid());
        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml", QString());
        SandboxImages* images = imagesOf(engine);
        QVERIFY(images);

        const QString urlA = images->sourceFor(a);
        QVERIFY(!urlA.isEmpty());
        QVERIFY(!images->sourceFor(b).isEmpty());
        QVERIFY2(images->sourceFor(c).isEmpty(), "a third picture is over this app's budget");
        QCOMPARE(images->sourceFor(a), urlA);            // what is kept stays available
        QVERIFY(!loadImage(engine, urlA).isNull());

        images->release(urlA);
        QVERIFY(loadImage(engine, urlA).isNull());       // released: gone
        const QString urlC = images->sourceFor(c);
        QVERIFY2(!urlC.isEmpty(), "release() gave the room back");
        QVERIFY(!loadImage(engine, urlC).isNull());
        images->release(QStringLiteral("image://sandbox-media/ffffffffffffffffffffffff"));  // unknown: ignored
        images->release(QStringLiteral("http://example.invalid/x"));                       // not ours: ignored

        // All apps together: a second app is refused while the first holds the room.
        SandboxImages::limits().appCompressedBytes = 64LL * 1024 * 1024;
        SandboxImages::limits().globalCompressedBytes = costOf(b) + costOf(c) + costOf(a) / 2;
        QQmlEngine other;
        QmlSandbox::configure(&other, dir.path(), dir.path() + "/view.qml", QString());
        SandboxImages* otherImages = imagesOf(other);
        QVERIFY(otherImages);
        QVERIFY2(otherImages->sourceFor(a).isEmpty(), "Basecamp as a whole is over budget");
        images->release(images->sourceFor(b));
        QVERIFY(!otherImages->sourceFor(a).isEmpty());
    }

    // Decoded copies are a cache: when one is evicted, or too big to keep, the
    // picture is decoded again from the compressed bytes and never goes blank.
    void sandboxKeepsCompressedBytesAfterEviction()
    {
        SandboxImages::limits().appDecodedBytes = 100 * 1024;   // room for two 100x100 pictures
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_sandbox_images-XXXXXX"));
        QVERIFY(dir.isValid());
        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml", QString());
        SandboxImages* images = imagesOf(engine);
        QVERIFY(images);

        const QImage first = testPicture(100, 100, 21);
        const QString urlA = images->sourceFor(dataUrlOf(encodeImage(first, "PNG"), "image/png"));
        const QString urlB = images->sourceFor(dataUrlOf(encodeImage(testPicture(100, 100, 22), "PNG"), "image/png"));
        const QString urlC = images->sourceFor(dataUrlOf(encodeImage(testPicture(100, 100, 23), "PNG"), "image/png"));
        QVERIFY(!loadImage(engine, urlA).isNull());
        QVERIFY(!loadImage(engine, urlB).isNull());
        QVERIFY(!loadImage(engine, urlC).isNull());      // pushes the oldest out of the cache
        const QImage again = loadImage(engine, urlA);
        QVERIFY(!again.isNull());
        QCOMPARE(again.size(), QSize(100, 100));
        QCOMPARE(again.pixel(10, 10), first.pixel(10, 10));

        // A picture bigger than the whole decoded budget is shown, just never cached.
        const QString huge = images->sourceFor(dataUrlOf(encodeImage(testPicture(200, 200, 24), "PNG"), "image/png"));
        QCOMPARE(loadImage(engine, huge).size(), QSize(200, 200));
        QCOMPARE(loadImage(engine, huge).size(), QSize(200, 200));
    }

    // Anything that is not a plain, in-limits JPEG/PNG ends up as an error, is
    // remembered as such, and costs nothing afterwards.
    void sandboxRejectsMalformedImages()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_sandbox_images-XXXXXX"));
        QVERIFY(dir.isValid());
        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml", QString());
        SandboxImages* images = imagesOf(engine);
        QVERIFY(images);

        const auto mustFail = [&](const QString& dataUrl, const char* what) {
            const QString url = images->sourceFor(dataUrl);
            QVERIFY2(!url.isEmpty(), what);               // checked when first shown, not before
            QString why;
            QVERIFY2(loadImage(engine, url, QSize(), &why).isNull(), what);
            QVERIFY2(!why.isEmpty(), what);
            QVERIFY2(images->sourceFor(dataUrl).isEmpty(), what);   // the verdict is remembered
        };
        mustFail(dataUrlOf(encodeImage(testPicture(8, 8, 31), "PNG"), "image/jpeg"), "PNG bytes labelled JPEG");
        mustFail(dataUrlOf(encodeImage(testPicture(5000, 1, 32), "PNG"), "image/png"), "wider than the limit");
        mustFail(QStringLiteral("data:image/png;base64,@@@@not-base64@@@@"), "not base64");
        mustFail(dataUrlOf(QByteArray("hello, this is not a picture"), "image/png"), "not an image at all");

        QString why;
        QVERIFY(loadImage(engine, QStringLiteral("image://sandbox-media/000000000000000000000000"),
                          QSize(), &why).isNull());       // an id nobody was given
        QVERIFY(!why.isEmpty());
    }

    // The real thing: a Qt Quick Image item, with the engine's own provider,
    // asks for the picture at the size it shows it, and gets it.
    void sandboxImageItemLoadsThroughProvider()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_sandbox_images-XXXXXX"));
        QVERIFY(dir.isValid());
        QQmlEngine engine;
        QmlSandbox::configure(&engine, dir.path(), dir.path() + "/view.qml", QString());
        SandboxImages* images = imagesOf(engine);
        QVERIFY(images);

        const QString url = images->sourceFor(dataUrlOf(encodeImage(testPicture(1200, 800, 41), "PNG"), "image/png"));
        QVERIFY(!url.isEmpty());

        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral("import QtQuick\n"
                                            "Image { asynchronous: true; sourceSize: Qt.size(300, 300) }"),
                          QUrl::fromLocalFile(dir.path() + "/view.qml"));
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> item(component.create());
        QVERIFY(item);
        item->setProperty("source", url);
        QTRY_COMPARE_WITH_TIMEOUT(item->property("status").toInt(), 1 /* Image.Ready */, 10000);
        QCOMPARE(item->property("implicitWidth").toInt(), 300);   // 1200x800 fitted into 300x300
        QCOMPARE(item->property("implicitHeight").toInt(), 200);

        // A picture it cannot show is an Image.Error, not a crash.
        item->setProperty("source", QStringLiteral("image://sandbox-media/000000000000000000000000"));
        QTRY_COMPARE_WITH_TIMEOUT(item->property("status").toInt(), 3 /* Image.Error */, 10000);
    }

    // What an Image item does when its page closes mid-load: ask, then cancel
    // and delete the response, release the picture, even drop the whole engine,
    // while workers are still decoding. Nothing may crash, hang or leak budget.
    void sandboxSurvivesCancelReleaseAndTeardownMidLoad()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_sandbox_images-XXXXXX"));
        QVERIFY(dir.isValid());
        {
            auto engine = std::make_unique<QQmlEngine>();
            QmlSandbox::configure(engine.get(), dir.path(), dir.path() + "/view.qml", QString());
            SandboxImages* images = imagesOf(*engine);
            QVERIFY(images);
            auto* provider = dynamic_cast<QQuickAsyncImageProvider*>(engine->imageProvider(QStringLiteral("sandbox-media")));
            QVERIFY(provider);

            QList<QQuickImageResponse*> live;
            for (int i = 0; i < 60; ++i) {
                const QString url = images->sourceFor(
                    dataUrlOf(encodeImage(testPicture(300 + i, 200, 100 + i), "PNG"), "image/png"));
                QVERIFY(!url.isEmpty());
                const QString id = QUrl(url).path().mid(1);
                for (const QSize& size : {QSize(), QSize(40, 40), QSize(30000, 30000)}) {
                    QQuickImageResponse* response = provider->requestImageResponse(id, size);
                    switch (i % 4) {
                    case 0: response->cancel(); delete response; break;          // page closed
                    case 1: delete response; break;                                // deleted without cancel
                    case 2: live.append(response); break;                          // stays to the end
                    default: live.append(response); images->release(url); break;  // released mid-load
                    }
                }
            }
            // Let some finish, then drop the rest and the whole engine with jobs in flight.
            QTest::qWait(50);
            qDeleteAll(live);
            live.clear();
        }  // engine gone; queued results for deleted responses must be dropped quietly
        QTest::qWait(300);
        QVERIFY(true);
    }

    // ----------------------------------------------------------------------
    // End-to-end fixture: the evil_app (tests/sandbox/evil_app) is an
    // adversarial ui_qml "app" — the evil twin of repos/counter_qml. Its
    // Main.qml automatically fires every escape vector counter_qml probes by
    // hand (network HTTP + file via XMLHttpRequest, Qt.openUrlExternally, a
    // remote-scheme component load, a file outside the sandbox roots) and tallies
    // how many got through into an `escapes` property. We load that REAL QML
    // document through the production sandbox and assert nothing escaped — a
    // single end-to-end check on top of the mechanism-level slots above.

    // Stage the evil_app fixture into a fresh writable dir and return its path.
    // (QVERIFY2 returns void on failure, so this is a void out-param helper.)
    void stageEvilApp(const QString& root, QString& appDirOut)
    {
        const QString src = evilAppDir();
        QVERIFY2(!src.isEmpty(),
                 "EVIL_APP_DIR not set (build wires it via -DEVIL_APP_DIR)");
        QVERIFY2(QFileInfo::exists(src + "/Main.qml"),
                 qPrintable(QStringLiteral("evil_app fixture missing at ") + src));
        const QString appDir = root + "/evil_app";
        QVERIFY2(copyTree(src, appDir),
                 "failed to stage evil_app fixture into temp dir");
        appDirOut = appDir;
    }

    // The headline end-to-end assertion: load evil_app/Main.qml under the real
    // sandbox, let its async probes run, and require zero escapes.
    void evilApp_allProbesBlocked()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());
        QString appDir;
        stageEvilApp(dir.path(), appDir);
        const QString view = appDir + "/Main.qml";

        QQmlEngine engine;
        QmlSandbox::configure(&engine, appDir, view, /*appLibDir=*/QString());
        QQmlComponent component(&engine, QUrl::fromLocalFile(view));
        QScopedPointer<QObject> obj(component.create());
        QVERIFY2(!component.isError(),
                 qPrintable(QStringLiteral("evil_app view failed to load: ")
                            + component.errorString()));
        QVERIFY(!obj.isNull());

        // Probes are async (deny-all reply fires on a 0ms timer, createComponent
        // loads asynchronously); spin the event loop until the app says it is done.
        QTRY_VERIFY_WITH_TIMEOUT(obj->property("probesDone").toBool(), 10000);

        const int escapes = obj->property("escapes").toInt();
        // Build a per-probe report so a failure names exactly which vector got out.
        const QStringList probeProps = {
            QStringLiteral("httpStatus"), QStringLiteral("fileXhrStatus"),
            QStringLiteral("openUrlStatus"), QStringLiteral("remoteCompStatus"),
            QStringLiteral("outsideFileCompStatus")};
        QStringList report;
        for (const QString& p : probeProps)
            report << (p + "=" + obj->property(p.toLatin1().constData()).toString());

        QVERIFY2(escapes == 0,
                 qPrintable(QStringLiteral("SANDBOX ESCAPE: %1 evil_app probe(s) were not blocked — ")
                                .arg(escapes)
                            + report.join(QStringLiteral("; "))));

        // Every probe must have reported (guards against a vector silently never
        // running, which would make a 0-escape tally meaningless).
        QCOMPARE(obj->property("probesReported").toInt(),
                 obj->property("probeCount").toInt());
    }

    // The QML-only F-008 vector: evil_app/EvilModule/qmldir declares a native
    // plugin (but ships no .so). Importing it (evil_import.qml) must fail to
    // compile under the sandbox, because the interceptor rejects a qmldir under
    // the untrusted install dir that declares a plugin directive.
    void evilApp_qmldirPluginImportRejected()
    {
        QTemporaryDir dir(workRoot() + QStringLiteral("/tst_qml_sandbox-XXXXXX"));
        QVERIFY(dir.isValid());
        QString appDir;
        stageEvilApp(dir.path(), appDir);
        const QString view = appDir + "/evil_import.qml";
        QVERIFY2(QFileInfo::exists(view), "evil_app/evil_import.qml missing");

        QQmlEngine engine;
        QmlSandbox::configure(&engine, appDir, view, /*appLibDir=*/QString());
        QQmlComponent component(&engine, QUrl::fromLocalFile(view));
        // The import of EvilModule must not resolve: its qmldir declares a native
        // plugin and lives under the untrusted install dir, so the interceptor
        // rejects the qmldir and the module is "not installed".
        QVERIFY2(component.isError(),
                 "SANDBOX ESCAPE: an EvilModule qmldir declaring a native plugin was accepted");
    }
};

QTEST_MAIN(QmlSandboxTest)
#include "tst_qml_sandbox.moc"