// srcdeps: OnboardingState.cpp
//
// Unit tests for first-run onboarding's persisted state (app/
// OnboardingState.{h,cpp}). Each check reads through a fresh call, as a
// restart would.
//
// Run: nix build .#unit-tests -L

#include "../app/OnboardingState.h"

#include <QtTest/QtTest>
#include <QTemporaryDir>

class OnboardingStateTest : public QObject {
    Q_OBJECT

    static void touch(const QString& path)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }

private slots:
    void a_new_user_dir_needs_onboarding()
    {
        QTemporaryDir dir;
        QVERIFY(OnboardingState::required(dir.path()));
        // Asking does not answer it.
        QVERIFY(OnboardingState::required(dir.path()));
    }

    void completed_onboarding_stays_completed()
    {
        QTemporaryDir dir;
        OnboardingState::markCompleted(dir.path());
        QVERIFY(!OnboardingState::required(dir.path()));
    }

    // The host makes these on every load, before onboarding is asked about:
    // they say nothing about an earlier run.
    void a_fresh_launchs_own_files_do_not_count_as_used()
    {
        QTemporaryDir dir;
        QDir base(dir.path());
        QVERIFY(base.mkpath(QStringLiteral("module_data/package_downloader/d88cf51908d6")));
        QVERIFY(base.mkpath(QStringLiteral("modules")));
        QVERIFY(base.mkpath(QStringLiteral("logs")));
        touch(base.filePath(QStringLiteral("logs/basecamp_20261022_090821.log")));

        QVERIFY(OnboardingState::required(dir.path()));
    }

    // An existing user keeps their saved download source rather than being
    // asked again after the upgrade that added onboarding.
    void a_previous_session_skips_onboarding()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("logs")));
        touch(dir.filePath(QStringLiteral("logs/basecamp_20261001_120000.log")));
        touch(dir.filePath(QStringLiteral("logs/basecamp_20261022_090821.log")));

        QVERIFY(!OnboardingState::required(dir.path()));

        // Recorded, so it holds once the old logs rotate away.
        QVERIFY(QDir(dir.filePath(QStringLiteral("logs"))).removeRecursively());
        QVERIFY(!OnboardingState::required(dir.path()));
    }

    void an_installed_module_skips_onboarding()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("modules/wallet_module")));
        QVERIFY(!OnboardingState::required(dir.path()));
    }

    void a_saved_downloader_config_skips_onboarding()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("module_data/package_downloader/d88cf51908d6")));
        touch(dir.filePath(QStringLiteral("module_data/package_downloader/d88cf51908d6/repositories.json")));
        QVERIFY(!OnboardingState::required(dir.path()));
    }

    void a_user_dir_that_does_not_exist_yet_can_be_completed()
    {
        QTemporaryDir dir;
        const QString base = dir.filePath(QStringLiteral("not/yet/there"));
        OnboardingState::markCompleted(base);
        QVERIFY(!OnboardingState::required(base));
    }
};

QTEST_GUILESS_MAIN(OnboardingStateTest)
#include "onboarding_state_test.moc"
