// Verifies the half of the mock that is still Basecamp's job: resolving the
// compiled-in fixture and materialising it where OTHER IMAGES can read it.
//
// Why that is worth a test. MockStore and the SDK mode flag are per-image
// statics; a module plugin that links logos-protocol statically carries its own
// copies, so nothing this process does in memory reaches it. The resolved file
// plus LOGOS_MOCK_FIXTURE is the ENTIRE contract between Basecamp and every
// other image. When it breaks, ui_qml apps render empty and the only symptom is
// "no expectation registered" in a child process's log.
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "MockBackendFixture.h"

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { qCritical() << "FAIL:" << m; ++failures; } \
                         else qInfo() << "ok  :" << m; } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    CHECK(qgetenv("LOGOS_MOCK_FIXTURE").isEmpty(),
          "LOGOS_MOCK_FIXTURE unset before install()");

    MockBackendFixture::install();

    const QString path = MockBackendFixture::resolvedFixturePath();
    CHECK(!path.isEmpty(), "install() reports a resolved fixture path");
    CHECK(QFile::exists(path), "the resolved fixture exists on disk");
    CHECK(QString::fromUtf8(qgetenv("LOGOS_MOCK_FIXTURE")) == path,
          "LOGOS_MOCK_FIXTURE exported, so child processes inherit it");

    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly), "resolved fixture is readable");
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    CHECK(doc.isObject(), "resolved fixture is valid JSON");

    const QJsonObject root  = doc.object();
    const QJsonArray modules = root.value("modules").toArray();
    const QJsonObject calls  = root.value("calls").toObject();
    CHECK(!modules.isEmpty(), "resolved fixture kept its `modules` section");
    CHECK(!calls.isEmpty(),   "resolved fixture kept its `calls` section");

    // The invariant PackageCoordinator.cpp depends on: it gates ALL of
    // package_manager's directory setup and all ten event subscriptions on this
    // module being reported as loaded.
    bool pmLoaded = false;
    for (const QJsonValue& v : modules) {
        const QJsonObject m = v.toObject();
        if (m.value("name").toString() == "package_manager")
            pmLoaded = m.value("loaded").toBool();
    }
    CHECK(pmLoaded, "package_manager marked loaded (PackageCoordinator gate)");

    // NO placeholder may survive into the written file. Plugin images read it
    // with no idea where this application installed anything, so an unexpanded
    // {PLUGINS_DIR} reaches them as a literal and fails far from here.
    const QString raw = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    for (const char* token : {"{PLUGINS_DIR}", "{MODULES_DIR}", "{APP_DIR}", "{LIB_EXT}"}) {
        CHECK(!raw.contains(QString::fromLatin1(token)),
              QStringLiteral("no unexpanded %1 in the resolved fixture")
                  .arg(QString::fromLatin1(token)));
    }

    // The ui_qml row must survive expansion intact, or UIPluginManager's
    // onUiPluginsFetched drops it with no error at all.
    const QJsonArray uiPlugins =
        calls.value("package_manager.getInstalledUiPlugins").toArray();
    CHECK(uiPlugins.size() == 1, "one ui plugin in the resolved fixture");
    const QJsonObject pmui = uiPlugins.at(0).toObject();
    CHECK(pmui.value("type").toString() == QLatin1String("ui_qml"), "pmui type survived");
    CHECK(!pmui.value("view").toString().isEmpty(),
          "pmui `view` non-empty (else onUiPluginsFetched drops the row)");

    const QString installDir = pmui.value("installDir").toString();
    CHECK(installDir.startsWith(QLatin1Char('/')),
          QStringLiteral("installDir expanded to an absolute path (%1)").arg(installDir));

    // hasBackendPlugin tests the EXTENSION, so a wrong suffix passes that gate
    // and only fails later inside ui-host as "The shared library was not found."
    const QString mainFile = pmui.value("mainFilePath").toString();
#if defined(Q_OS_WIN)
    const QString expectedExt = QStringLiteral(".dll");
#elif defined(Q_OS_MAC)
    const QString expectedExt = QStringLiteral(".dylib");
#else
    const QString expectedExt = QStringLiteral(".so");
#endif
    CHECK(mainFile.endsWith(expectedExt),
          QStringLiteral("mainFilePath uses this platform's %1 (got %2)")
              .arg(expectedExt, mainFile));

    // Idempotent: a second call must not rewrite or repoint anything.
    MockBackendFixture::install();
    CHECK(MockBackendFixture::resolvedFixturePath() == path,
          "install() is idempotent");

    qInfo() << (failures ? "=== FAILURES:" : "=== ALL PASSED, failures =") << failures;
    return failures ? 1 : 0;
}
