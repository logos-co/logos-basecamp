#include "PackageInstallDecision.h"

#include <QtTest/QtTest>

class PackageInstallDecisionTest : public QObject {
    Q_OBJECT

private slots:
    void embeddedPackageKeepsBundledCopy()
    {
        const QVariantList installed{
            QVariantMap{{"name", "storage_module"},
                        {"installType", "embedded"}}};
        QVERIFY(!shouldRemoveBeforeInstall(installed, "storage_module"));
    }

    void userPackageIsRemovedBeforeReplacement()
    {
        const QVariantList installed{
            QVariantMap{{"name", "storage_module"},
                        {"installType", "user"}}};
        QVERIFY(shouldRemoveBeforeInstall(installed, "storage_module"));
    }

    void moduleNameIsAcceptedWhenItDiffersFromPackageName()
    {
        const QVariantList installed{
            QVariantMap{{"name", "storage"},
                        {"moduleName", "storage_module"},
                        {"installType", "embedded"}}};
        QVERIFY(!shouldRemoveBeforeInstall(installed, "storage_module"));
    }

    void missingPackageDoesNotTriggerRemoval()
    {
        QVERIFY(!shouldRemoveBeforeInstall({}, "storage_module"));
    }
};

QTEST_GUILESS_MAIN(PackageInstallDecisionTest)
#include "package_install_decision_test.moc"
