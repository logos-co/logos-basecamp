// srcdeps: links/SchemeRegistrar.cpp links/LinkUrl.cpp
//
// LOGOS_NO_SCHEME_REGISTER's value parsing, and the shape of the command the
// registration actually hands the OS.
//
// The env flag existed as a bare qEnvironmentVariableIsSet, which made `=0`
// SKIP registration — a flag whose off switch turns it on. It fails in the
// direction hardest to notice: links quietly stop working, and the variable you
// set to fix that is the thing breaking it. The table below is the whole reason
// the parsing was split out of the anonymous namespace.
//
// The Exec case covers the other way this feature fails while looking healthy.
// The desktop entry was written with a bare `%u`, which hands the URL over as a
// POSITIONAL argument — and main() reads it only from --uri, never from a
// positional, because a scheme handler's value is attacker-influenced and one
// that happened to look like a flag must not be read as one. Every layer
// worked: the entry registered, the browser launched the app, argv carried the
// URL, and nothing consumed it. Nothing failed loudly enough to notice, and no
// test looked at the generated entry at all.

#include "links/SchemeRegistrar.h"

#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

class TestSchemeRegistrar : public QObject
{
    Q_OBJECT

private slots:
    void testValueMeansSkip_data();
    void testValueMeansSkip();
#if defined(Q_OS_LINUX)
    void testDesktopEntryPassesTheUrlAsTheUriOption();
#endif
};

void TestSchemeRegistrar::testValueMeansSkip_data()
{
    QTest::addColumn<QString>("value");
    QTest::addColumn<bool>("skip");

    // Off — "register normally".
    QTest::newRow("empty")        << QString()                    << false;
    QTest::newRow("zero")         << QStringLiteral("0")          << false;
    QTest::newRow("false")        << QStringLiteral("false")      << false;
    QTest::newRow("FALSE")        << QStringLiteral("FALSE")      << false;
    QTest::newRow("no")           << QStringLiteral("no")         << false;
    QTest::newRow("off")          << QStringLiteral("off")        << false;
    QTest::newRow("padded false") << QStringLiteral("  False  ")  << false;

    // On — skip registration.
    QTest::newRow("one")          << QStringLiteral("1")          << true;
    QTest::newRow("true")         << QStringLiteral("true")       << true;
    QTest::newRow("YES")          << QStringLiteral("YES")        << true;
    // Anything unrecognised is ON. A typo means the flag you were reaching for
    // takes effect, rather than silently doing nothing.
    QTest::newRow("garbage")      << QStringLiteral("ture")       << true;
}

void TestSchemeRegistrar::testValueMeansSkip()
{
    QFETCH(QString, value);
    QFETCH(bool, skip);
    QCOMPARE(SchemeRegistrar::valueMeansSkip(value), skip);
}

#if defined(Q_OS_LINUX)
void TestSchemeRegistrar::testDesktopEntryPassesTheUrlAsTheUriOption()
{
    // Point GenericDataLocation at a throwaway tree, so this writes nowhere
    // near the developer's real basecamp:// handler.
    //
    // XDG_DATA_HOME rather than QStandardPaths::setTestModeEnabled(), which
    // anchors on $HOME — unwritable under the nix build sandbox
    // (/homeless-shelter), where this suite normally runs.
    QTemporaryDir dataHome;
    QVERIFY(dataHome.isValid());
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());

    // The suite may well be running under the same flag the doc-tests set.
    qunsetenv("LOGOS_NO_SCHEME_REGISTER");
    // launcherPath() prefers $APPIMAGE, which would make the Exec below name
    // something other than this test binary.
    qunsetenv("APPIMAGE");

    const QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                       + QStringLiteral("/applications/logos-basecamp.desktop");
    QVERIFY2(path.startsWith(dataHome.path()), qPrintable(path));

    QVERIFY(SchemeRegistrar::registerScheme());
    QVERIFY2(QFile::exists(path), qPrintable(path));

    QFile entry(path);
    QVERIFY(entry.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(entry.readAll());

    QString exec;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("Exec="))) {
            exec = line;
            break;
        }
    }
    QVERIFY2(!exec.isEmpty(), qPrintable(text));

    // The field code has to be attached to --uri. Asserting "contains %u" is
    // what a test written alongside the bug would have said, and it passes
    // against the bug.
    QCOMPARE(exec, QStringLiteral("Exec=\"%1\" --uri=%u")
                       .arg(QCoreApplication::applicationFilePath()));

    // Without this the entry is not a scheme handler at all, whatever Exec says.
    QVERIFY2(text.contains(QLatin1String("MimeType=x-scheme-handler/basecamp;")),
             qPrintable(text));
}
#endif

QTEST_MAIN(TestSchemeRegistrar)
#include "scheme_registrar_test.moc"
