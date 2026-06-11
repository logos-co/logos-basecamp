#include <QtQuickTest/quicktest.h>
#include <QtQml/qqml.h>

#include "AppsFilterProxy.h"
#include "InstallEnums.h"

class Setup : public QObject {
    Q_OBJECT
public:
    Setup() = default;

public slots:
    void qmlEngineAvailable(QQmlEngine* /*engine*/)
    {
        qmlRegisterUncreatableType<InstallStage>("Basecamp.Backend", 1, 0,
            "InstallStage",
            QStringLiteral("Use InstallStage.Downloading etc.; not instantiable."));
        qmlRegisterUncreatableType<InstallStatus>("Basecamp.Backend", 1, 0,
            "InstallStatus",
            QStringLiteral("Use InstallStatus.Installed etc.; not instantiable."));
        qmlRegisterType<AppsFilterProxy>("Basecamp.Backend", 1, 0, "AppsFilterProxy");
    }
};

QUICK_TEST_MAIN_WITH_SETUP(qml_tests, Setup)

#include "main.moc"
