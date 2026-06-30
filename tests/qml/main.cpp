#include <QtQuickTest/quicktest.h>
#include <QtQml/qqml.h>

#include "ModuleFilterProxy.h"
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
        qmlRegisterType<ModuleFilterProxy>("Basecamp.Backend", 1, 0, "ModuleFilterProxy");
    }
};

QUICK_TEST_MAIN_WITH_SETUP(qml_tests, Setup)

#include "main.moc"
