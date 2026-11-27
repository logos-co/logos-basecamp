// srcdeps: utils/RevealInFileManager.cpp
//
// Unit tests for Settings → Dashboard's "Show in Finder / Explorer" button
// (app/utils/RevealInFileManager.{h,cpp}, called by
// MainUIBackend::openUserDirectory). Every platform's launch is checked on any
// host, against a recording launcher: nothing opens a file manager.
//
// Run: nix build .#unit-tests -L

#include "../app/utils/LogosBasecampPaths.h"
#include "../app/utils/RevealInFileManager.h"

#include <QtTest/QtTest>
#include <QTemporaryDir>

using namespace LogosBasecamp;

Q_DECLARE_METATYPE(LogosBasecamp::FileManagerPlatform)

namespace {

struct RecordingLauncher {
    QStringList programs;
    QList<QStringList> arguments;
    QList<QUrl> urls;
    bool result = true;

    FileManagerLauncher launcher()
    {
        return {
            [this](const QString& program, const QStringList& args) {
                programs << program;
                arguments << args;
                return result;
            },
            [this](const QUrl& url) {
                urls << url;
                return result;
            },
        };
    }
};

} // namespace

class RevealInFileManagerTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;
    // Spaces, as in ~/Library/Application Support: each must stay one argument.
    QString userDir() const { return m_root.path() + QStringLiteral("/my user dir/LogosBasecampDev"); }

    // The --user-dir path, as main.cpp passes it on.
    void useUserDir() { qputenv("LOGOS_USER_DIR", userDir().toUtf8()); }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_root.isValid());
    }
    void cleanup() { qunsetenv("LOGOS_USER_DIR"); }

    void current_platform_is_the_host()
    {
#if defined(Q_OS_MACOS)
        QCOMPARE(currentFileManagerPlatform(), FileManagerPlatform::MacOS);
#elif defined(Q_OS_WIN)
        QCOMPARE(currentFileManagerPlatform(), FileManagerPlatform::Windows);
#else
        QCOMPARE(currentFileManagerPlatform(), FileManagerPlatform::Other);
#endif
    }

    void macos_reveals_the_user_dir_in_finder()
    {
        useUserDir();
        RecordingLauncher rec;
        QVERIFY(revealInFileManager(LogosBasecampPaths::baseDirectory(),
                                    FileManagerPlatform::MacOS, rec.launcher()));
        QCOMPARE(rec.programs, QStringList{QStringLiteral("/usr/bin/open")});
        QCOMPARE(rec.arguments.value(0), (QStringList{QStringLiteral("-R"), userDir()}));
        QVERIFY(rec.urls.isEmpty());
    }

    void windows_selects_the_user_dir_in_explorer()
    {
        useUserDir();
        RecordingLauncher rec;
        QVERIFY(revealInFileManager(LogosBasecampPaths::baseDirectory(),
                                    FileManagerPlatform::Windows, rec.launcher()));
        QCOMPARE(rec.programs, QStringList{QStringLiteral("explorer.exe")});
        const QStringList args = rec.arguments.value(0);
        QCOMPARE(args.size(), 2);
        QCOMPARE(args.at(0), QStringLiteral("/select,"));
        QCOMPARE(args.at(1), QString(userDir()).replace(QLatin1Char('/'), QLatin1Char('\\')));
        QVERIFY(!args.at(1).contains(QLatin1Char('/')));
        QVERIFY(rec.urls.isEmpty());
    }

    void windows_passes_a_drive_path_with_backslashes()
    {
        const FileManagerLaunch launch = fileManagerLaunchFor(
            FileManagerPlatform::Windows, QStringLiteral("C:/Users/some one/AppData/Roaming/Logos/LogosBasecamp"));
        QCOMPARE(launch.arguments, (QStringList{QStringLiteral("/select,"),
                                                QStringLiteral("C:\\Users\\some one\\AppData\\Roaming\\Logos\\LogosBasecamp")}));
    }

    void linux_opens_the_user_dir_itself()
    {
        useUserDir();
        RecordingLauncher rec;
        QVERIFY(revealInFileManager(LogosBasecampPaths::baseDirectory(),
                                    FileManagerPlatform::Other, rec.launcher()));
        QVERIFY(rec.programs.isEmpty());
        QCOMPARE(rec.urls, QList<QUrl>{QUrl::fromLocalFile(userDir())});
        QCOMPARE(rec.urls.value(0).toLocalFile(), userDir());
    }

    void without_user_dir_it_shows_the_default_one_data()
    {
        QTest::addColumn<FileManagerPlatform>("platform");
        QTest::newRow("macos") << FileManagerPlatform::MacOS;
        QTest::newRow("windows") << FileManagerPlatform::Windows;
        QTest::newRow("linux") << FileManagerPlatform::Other;
    }
    void without_user_dir_it_shows_the_default_one()
    {
        QFETCH(FileManagerPlatform, platform);
        const QString dir = LogosBasecampPaths::baseDirectory();
        QVERIFY(dir != userDir());
        QCOMPARE(dir, LogosBasecampPaths::isPortableBuild()
                          ? LogosBasecampPaths::dataDirectory()
                          : LogosBasecampPaths::dataDirectory() + QStringLiteral("Dev"));
        // Not revealInFileManager: it would create the default dir, outside the sandbox.
        const FileManagerLaunch launch = fileManagerLaunchFor(platform, dir);
        switch (platform) {
        case FileManagerPlatform::MacOS:
            QCOMPARE(launch.arguments, (QStringList{QStringLiteral("-R"), dir}));
            break;
        case FileManagerPlatform::Windows:
            QCOMPARE(launch.arguments.value(1), QString(dir).replace(QLatin1Char('/'), QLatin1Char('\\')));
            break;
        case FileManagerPlatform::Other:
            QCOMPARE(launch.url, QUrl::fromLocalFile(dir));
            break;
        }
    }

    // A fresh --user-dir has no folder yet; the file manager can't show one.
    void a_missing_user_dir_is_created_first()
    {
        useUserDir();
        QDir(userDir()).removeRecursively();
        RecordingLauncher rec;
        QVERIFY(revealInFileManager(LogosBasecampPaths::baseDirectory(),
                                    FileManagerPlatform::Other, rec.launcher()));
        QVERIFY(QFileInfo(userDir()).isDir());
    }

    void a_launcher_that_fails_is_reported_data()
    {
        without_user_dir_it_shows_the_default_one_data();
    }
    void a_launcher_that_fails_is_reported()
    {
        QFETCH(FileManagerPlatform, platform);
        useUserDir();
        RecordingLauncher rec;
        rec.result = false;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Couldn't open the user directory"));
        QVERIFY(!revealInFileManager(LogosBasecampPaths::baseDirectory(), platform, rec.launcher()));
        QCOMPARE(rec.programs.size() + rec.urls.size(), 1);
    }

    void a_dir_that_cant_be_created_launches_nothing()
    {
        QFile blocker(m_root.path() + QStringLiteral("/a-file"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        qputenv("LOGOS_USER_DIR", (blocker.fileName() + QStringLiteral("/user")).toUtf8());
        RecordingLauncher rec;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Couldn't create the user directory"));
        QVERIFY(!revealInFileManager(LogosBasecampPaths::baseDirectory(),
                                     FileManagerPlatform::MacOS, rec.launcher()));
        QVERIFY(rec.programs.isEmpty());
        QVERIFY(rec.urls.isEmpty());
    }
};

QTEST_GUILESS_MAIN(RevealInFileManagerTest)
#include "reveal_in_file_manager_test.moc"
