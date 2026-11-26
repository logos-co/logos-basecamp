// srcdeps: ShellIntents.cpp IntentRegistry.cpp utils/ManifestFile.cpp
//
// The default-app table: every grant a default app holds comes from its slot's
// row, and the shell's restrictions are the table's.

#include "DefaultApps.h"
#include "IntentRegistry.h"
#include "ShellIntents.h"
#include "ShellSections.h"
#include "utils/BuiltInApps.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTest>

using namespace logos::defaultapps;

class TestDefaultApps : public QObject
{
    Q_OBJECT

private slots:
    void theInstallerAppsHoldTheirSlots()
    {
        QCOMPARE(apps(), (QStringList{QStringLiteral("app_manager_ui"), QStringLiteral("package_manager_ui")}));
        const DefaultApp* packages = appInSlot(Slot::Packages);
        QVERIFY(packages);
        QCOMPARE(packages->app, QStringLiteral("package_manager_ui"));
        QCOMPARE(packages->section, int(ShellSection::PackageManager));
        const DefaultApp* apps_ = appInSlot(Slot::Apps);
        QVERIFY(apps_);
        QCOMPARE(apps_->app, QStringLiteral("app_manager_ui"));
        QCOMPARE(apps_->section, int(ShellSection::AppManager));
        QCOMPARE(logos::builtin::apps(), apps());
    }

    // Each app through its screen, and its backend raising it itself.
    void onlyTheSlotsMayAskForTheConfirmIntents()
    {
        const QStringList restricted = restrictedIntents();
        QCOMPARE(QSet<QString>(restricted.cbegin(), restricted.cend()),
                 QSet<QString>(ShellIntents::kPackageConfirmIntents.cbegin(),
                               ShellIntents::kPackageConfirmIntents.cend()));
        for (const QString& intent : ShellIntents::kPackageConfirmIntents) {
            QCOMPARE(requestersFor(intent),
                     (QStringList{QStringLiteral("app_manager_ui"), QStringLiteral("app_manager_ui_backend"),
                                  QStringLiteral("package_manager_ui"),
                                  QStringLiteral("package_manager_ui_backend")}));
            QCOMPARE(backendsFor(intent),
                     (QStringList{QStringLiteral("app_manager_ui_backend"),
                                  QStringLiteral("package_manager_ui_backend")}));
        }
        QVERIFY(requestersFor(QStringLiteral("basecamp.apps.open")).isEmpty());
        QVERIFY(backendsFor(QStringLiteral("basecamp.apps.open")).isEmpty());
    }

    // intents_module knows modules: the declaration names the backends.
    void theModuleDeclarationRestrictsToTheSlotsBackends()
    {
        const QJsonObject restricted = ShellIntents::moduleDeclaration(
            QStringLiteral("main_ui"), QStringLiteral("Logos"), QStringLiteral("qrc:/x.svg"),
            QStringLiteral("logos_link")).value(QStringLiteral("restricted")).toObject();
        QCOMPARE(restricted.keys().size(), ShellIntents::kPackageConfirmIntents.size());
        for (const QString& intent : ShellIntents::kPackageConfirmIntents) {
            QCOMPARE(restricted.value(intent).toArray(),
                     (QJsonArray{QStringLiteral("app_manager_ui_backend"),
                                 QStringLiteral("package_manager_ui_backend")}));
        }
    }

    // Basecamp's own Install goes to App Manager, its Details to Package Manager.
    void basecampsOwnRequestsHaveADefaultProvider()
    {
        QCOMPARE(defaultProviderFor(QStringLiteral("packages.install")),
                 QStringLiteral("app_manager_ui_backend"));
        QCOMPARE(defaultProviderFor(QStringLiteral("apps.search")),
                 QStringLiteral("app_manager_ui_backend"));
        QCOMPARE(defaultProviderFor(QStringLiteral("packages.show")),
                 QStringLiteral("package_manager_ui_backend"));
        QVERIFY(defaultProviderFor(QStringLiteral("wallet.send")).isEmpty());
    }

    void theShellRestrictsWhatTheTableRestricts()
    {
        IntentRegistry registry;
        ShellIntents::registerWith(&registry, QStringLiteral("main_ui"), QStringLiteral("Logos"),
                                   QStringLiteral("qrc:/x.svg"));
        for (const QString& intent : ShellIntents::kPackageConfirmIntents) {
            QVERIFY2(registry.requesterAllowed(intent, QStringLiteral("package_manager_ui")), qPrintable(intent));
            QVERIFY2(registry.requesterAllowed(intent, QStringLiteral("package_manager_ui_backend")),
                     qPrintable(intent));
            QVERIFY2(registry.requesterAllowed(intent, QStringLiteral("app_manager_ui")), qPrintable(intent));
            QVERIFY2(registry.requesterAllowed(intent, QStringLiteral("app_manager_ui_backend")),
                     qPrintable(intent));
            // Install too: a third party asks App Manager or Package Manager
            // (packages.install), never Basecamp directly.
            QVERIFY2(!registry.requesterAllowed(intent, QStringLiteral("wallet_ui")), qPrintable(intent));
            QVERIFY2(!registry.requesterAllowed(intent, QStringLiteral("wallet_ui_backend")), qPrintable(intent));
            QVERIFY2(!registry.requesterAllowed(intent, QStringLiteral("Package_Manager_UI")), qPrintable(intent));
        }
        for (const QString& intent : ShellIntents::kNavigationIntents)
            QVERIFY2(registry.requesterAllowed(intent, QStringLiteral("wallet_ui")), qPrintable(intent));
    }

    void packageModuleCallersAreCoreAndTheSlotsBackends()
    {
        QCOMPARE(packageModuleCallers(),
                 (QStringList{QStringLiteral("core"), QStringLiteral("app_manager_ui_backend"),
                              QStringLiteral("package_manager_ui_backend")}));
    }

    // Only Package Manager installs from a file.
    void onlyThePackagesSlotNamesLocalFiles()
    {
        QVERIFY(namesLocalFiles(QStringLiteral("package_manager_ui")));
        QVERIFY(namesLocalFiles(QStringLiteral("package_manager_ui_backend")));
        QVERIFY(!namesLocalFiles(QStringLiteral("app_manager_ui")));
        QVERIFY(!namesLocalFiles(QStringLiteral("app_manager_ui_backend")));
        QVERIFY(!namesLocalFiles(QStringLiteral("wallet_ui")));
        QVERIFY(!namesLocalFiles(QStringLiteral("Package_Manager_UI")));
    }

    // Each app is shown as its section; a backend is not an app.
    void theSectionIsTheApps()
    {
        QCOMPARE(sectionOf(QStringLiteral("package_manager_ui")), int(ShellSection::PackageManager));
        QCOMPARE(sectionOf(QStringLiteral("app_manager_ui")), int(ShellSection::AppManager));
        QCOMPARE(sectionOf(QStringLiteral("package_manager_ui_backend")), -1);
        QCOMPARE(sectionOf(QStringLiteral("app_manager_ui_backend")), -1);
        QCOMPARE(sectionOf(QStringLiteral("wallet_ui")), -1);
        QCOMPARE(appInSection(ShellSection::PackageManager)->app, QStringLiteral("package_manager_ui"));
        QCOMPARE(appInSection(ShellSection::AppManager)->app, QStringLiteral("app_manager_ui"));
        QVERIFY(!appInSection(ShellSection::Settings));
        QVERIFY(!appInSection(-1));
    }

    // A second slot takes one row; every grant follows from it.
    void anotherSlotIsOneRow()
    {
        const QList<DefaultApp> rows{
            {Slot::Packages, QStringLiteral("package_manager_ui"), ShellSection::PackageManager,
             {QStringLiteral("basecamp.packages.confirm_install")}, true, true},
            {Slot::Packages, QStringLiteral("viewer_ui"), ShellSection::Settings,
             {QStringLiteral("basecamp.packages.confirm_install")}, false, false},
        };
        QCOMPARE(apps(rows), (QStringList{QStringLiteral("package_manager_ui"), QStringLiteral("viewer_ui")}));
        QCOMPARE(requestersFor(QStringLiteral("basecamp.packages.confirm_install"), rows),
                 (QStringList{QStringLiteral("package_manager_ui"), QStringLiteral("package_manager_ui_backend"),
                              QStringLiteral("viewer_ui"), QStringLiteral("viewer_ui_backend")}));
        QCOMPARE(packageModuleCallers(rows),
                 (QStringList{QStringLiteral("core"), QStringLiteral("package_manager_ui_backend")}));
        QVERIFY(!namesLocalFiles(QStringLiteral("viewer_ui"), rows));
        QCOMPARE(sectionOf(QStringLiteral("viewer_ui"), rows), int(ShellSection::Settings));
    }

    // The table itself: each app once, each section once, only confirm intents.
    void theTableIsConsistent()
    {
        QSet<QString> names;
        QSet<int> sections;
        for (const DefaultApp& row : table()) {
            QVERIFY2(!row.app.endsWith(QStringLiteral("_backend")), qPrintable(row.app));
            QVERIFY2(!names.contains(row.app), qPrintable(row.app));
            names.insert(row.app);
            if (row.section >= 0) {
                QVERIFY(ShellSection::isValid(row.section));
                QVERIFY2(!sections.contains(row.section), qPrintable(row.app));
                sections.insert(row.section);
            }
            for (const QString& intent : row.mayAsk)
                QVERIFY2(ShellIntents::kPackageConfirmIntents.contains(intent), qPrintable(intent));
        }
    }
};

QTEST_GUILESS_MAIN(TestDefaultApps)
#include "default_apps_test.moc"
