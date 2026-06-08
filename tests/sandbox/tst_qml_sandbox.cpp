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

#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
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
};

QTEST_MAIN(QmlSandboxTest)
#include "tst_qml_sandbox.moc"
