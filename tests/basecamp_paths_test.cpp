#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "../app/utils/LogosBasecampPaths.h"

class BasecampPathsTest : public QObject
{
    Q_OBJECT

private:
    QByteArray m_originalUserDir;
    bool m_hadOriginalUserDir = false;
    QString m_originalOrganizationName;
    QString m_originalApplicationName;

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void defaultRootIsExportedForUiHost();
    void explicitOverrideIsExportedVerbatim();
};

void BasecampPathsTest::initTestCase()
{
    m_hadOriginalUserDir = qEnvironmentVariableIsSet("LOGOS_USER_DIR");
    m_originalUserDir = qgetenv("LOGOS_USER_DIR");
    m_originalOrganizationName = QCoreApplication::organizationName();
    m_originalApplicationName = QCoreApplication::applicationName();

    // Keep the default-root regression hermetic: Qt redirects standard paths
    // to its test tree instead of touching the developer's real app data.
    QStandardPaths::setTestModeEnabled(true);
}

void BasecampPathsTest::cleanupTestCase()
{
    QCoreApplication::setOrganizationName(m_originalOrganizationName);
    QCoreApplication::setApplicationName(m_originalApplicationName);
    if (m_hadOriginalUserDir)
        qputenv("LOGOS_USER_DIR", m_originalUserDir);
    else
        qunsetenv("LOGOS_USER_DIR");
}

void BasecampPathsTest::init()
{
    qunsetenv("LOGOS_USER_DIR");
    QCoreApplication::setOrganizationName(QStringLiteral("LogosProfileTest"));
    QCoreApplication::setApplicationName(QStringLiteral("LogosBasecamp"));
}

void BasecampPathsTest::cleanup()
{
    qunsetenv("LOGOS_USER_DIR");
}

void BasecampPathsTest::defaultRootIsExportedForUiHost()
{
    const QString parentRoot = LogosBasecampPaths::baseDirectory();
    QVERIFY2(!parentRoot.isEmpty(), "Qt must resolve a test-mode Basecamp root");
    QVERIFY(!qEnvironmentVariableIsSet("LOGOS_USER_DIR"));

    QCOMPARE(LogosBasecampPaths::resolveAndExportBaseDirectory(), parentRoot);
    QCOMPARE(qEnvironmentVariable("LOGOS_USER_DIR"), parentRoot);
    QCOMPARE(QProcessEnvironment::systemEnvironment().value(
                 QStringLiteral("LOGOS_USER_DIR")),
             parentRoot);

    // Model the child process changing Qt's application identity to ui-host.
    // The inherited explicit root must win over its own AppDataLocation.
    QCoreApplication::setApplicationName(QStringLiteral("ui-host"));
    QCOMPARE(LogosBasecampPaths::baseDirectory(), parentRoot);
}

void BasecampPathsTest::explicitOverrideIsExportedVerbatim()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString explicitRoot = profile.path() + QStringLiteral("/profile-a");
    qputenv("LOGOS_USER_DIR", explicitRoot.toUtf8());

    QCOMPARE(LogosBasecampPaths::resolveAndExportBaseDirectory(), explicitRoot);
    QCOMPARE(qEnvironmentVariable("LOGOS_USER_DIR"), explicitRoot);
    QCOMPARE(QProcessEnvironment::systemEnvironment().value(
                 QStringLiteral("LOGOS_USER_DIR")),
             explicitRoot);

    QCoreApplication::setApplicationName(QStringLiteral("ui-host"));
    QCOMPARE(LogosBasecampPaths::baseDirectory(), explicitRoot);
}

QTEST_GUILESS_MAIN(BasecampPathsTest)
#include "basecamp_paths_test.moc"
