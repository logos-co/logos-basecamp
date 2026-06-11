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

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>

#include "restricted/QmlSandbox.h"

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
