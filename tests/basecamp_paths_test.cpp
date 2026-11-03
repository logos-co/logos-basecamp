//
// Unit tests for where Basecamp keeps the user's data (app/utils/
// LogosBasecampPaths.h). An isolated instance (--user-dir, which sets
// LOGOS_USER_DIR) must keep everything under its own directory, themes
// included, or it reads and changes the user's real ones.
//
// Run: nix build .#unit-tests -L

#include "../app/utils/LogosBasecampPaths.h"

#include <QtTest/QtTest>

class BasecampPathsTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup() { qunsetenv("LOGOS_USER_DIR"); }

    void themes_follow_the_user_dir()
    {
        qputenv("LOGOS_USER_DIR", "/tmp/isolated-basecamp");
        QCOMPARE(LogosBasecampPaths::themesDirectory(), QStringLiteral("/tmp/isolated-basecamp/themes"));
        QCOMPARE(LogosBasecampPaths::modulesDirectory(), QStringLiteral("/tmp/isolated-basecamp/modules"));
    }

    void themes_sit_beside_the_other_user_data()
    {
        QCOMPARE(LogosBasecampPaths::themesDirectory(),
                 LogosBasecampPaths::baseDirectory() + QStringLiteral("/themes"));
    }
};

QTEST_GUILESS_MAIN(BasecampPathsTest)
#include "basecamp_paths_test.moc"
