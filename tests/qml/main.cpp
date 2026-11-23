#include <QtQuickTest/quicktest.h>
#include <QtQml/qqml.h>
#include <QCoreApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#ifdef HAVE_LOGOS_DESIGN_SYSTEM
#include <ThemeManager.h>
#include <ThemeStore.h>
#endif

#include "AppsFilterProxy.h"
#include "AppsModel.h"
#include "ModulesFilterProxy.h"
#include "InstallEnums.h"

// The App Manager's model, filled by a test as the backend fills it.
class TestAppsModel : public AppsModel {
    Q_OBJECT
public:
    using AppsModel::AppsModel;
    Q_INVOKABLE void setCatalog(const QVariantList& rows) { replaceCatalog(rows); }
    Q_INVOKABLE void install(const QString& name, const QString& version, const QString& installType)
    {
        markInstalled(name, version);
        setInstallType(name, installType);
    }
};

class Setup : public QObject {
    Q_OBJECT
public:
    Setup()
    {
        // The theme store saves the chosen theme; tst_AppearanceView reads
        // its theme files from fixtures/themes.
        QCoreApplication::setOrganizationName("Logos");
        QCoreApplication::setApplicationName("BasecampQmlTests");
#ifdef HAVE_LOGOS_DESIGN_SYSTEM
        Logos::ThemeStore::instance()->addDirectory(
            qEnvironmentVariable("QUICK_TEST_SOURCE_DIR", QUICK_TEST_SOURCE_DIR)
            + "/fixtures/themes");
#endif
    }

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
#ifdef HAVE_LOGOS_DESIGN_SYSTEM
        // The host's theme object, as MainContainer gives it to ContentViews.
        engine->rootContext()->setContextProperty("themeManager", &m_themeManager);
#else
        Q_UNUSED(engine);
#endif
        qmlRegisterUncreatableType<InstallStage>("Basecamp.Backend", 1, 0,
            "InstallStage",
            QStringLiteral("Use InstallStage.Downloading etc.; not instantiable."));
        qmlRegisterUncreatableType<InstallStatus>("Basecamp.Backend", 1, 0,
            "InstallStatus",
            QStringLiteral("Use InstallStatus.Installed etc.; not instantiable."));
        qmlRegisterType<AppsFilterProxy>("Basecamp.Backend", 1, 0, "AppsFilterProxy");
        qmlRegisterType<ModulesFilterProxy>("Basecamp.Backend", 1, 0, "ModulesFilterProxy");
        qmlRegisterType<TestAppsModel>("BasecampTests", 1, 0, "TestAppsModel");
    }

#ifdef HAVE_LOGOS_DESIGN_SYSTEM
private:
    QTemporaryDir m_installDir;
    Logos::ThemeManager m_themeManager{ m_installDir.path() };
#endif
};

QUICK_TEST_MAIN_WITH_SETUP(qml_tests, Setup)

#include "main.moc"
