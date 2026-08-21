// srcdeps: LogLineParser.cpp
//
// Unit tests for LogLineParser — the classifier behind Settings → Logs. One
// case per line shape found in real session logs, plus the file-name
// decomposition the file list relies on. Plain QtTest, glob-discovered by
// tests/CMakeLists.txt.

#include "LogLine.h"
#include "LogosBasecampPaths.h"

#include <QtTest/QtTest>

class LogLineParserTest : public QObject {
    Q_OBJECT

private slots:
    void coreModuleLine()
    {
        const auto lines = LogLineParser::parse(
            "[2026-08-21 10:04:23.875] [info] [logos] [package_downloader] "
            "ModuleProxy: callRemoteMethod \"listRepositories\" args: 0");
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].timestamp, QString("2026-08-21 10:04:23.875"));
        QCOMPARE(lines[0].level, QString("info"));
        QCOMPARE(lines[0].source, QString("package_downloader"));
        QCOMPARE(lines[0].message,
                 QString("ModuleProxy: callRemoteMethod \"listRepositories\" args: 0"));
    }

    void nestedTagStaysInMessage()
    {
        const auto lines = LogLineParser::parse(
            "[2026-08-21 10:07:30.120] [info] [logos] [package_downloader] [LogosProviderObject] "
            "ModuleProxy: forwarding event \"catalogChanged\" as Qt signal");
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].source, QString("package_downloader"));
        QVERIFY(lines[0].message.startsWith("[LogosProviderObject] ModuleProxy"));
    }

    void liblogosLineWithoutModule()
    {
        const auto lines = LogLineParser::parse(
            "[2026-08-21 10:04:16.275] [warning] [logos] Process did not terminate gracefully, "
            "killing: package_downloader");
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].level, QString("warning"));
        QCOMPARE(lines[0].source, LogLineNames::kSourceCore);
        QVERIFY(lines[0].message.startsWith("Process did not terminate"));
    }

    void moduleStdoutLineStripsAnsi()
    {
        const auto lines = LogLineParser::parse(
            "[2026-08-21 10:35:27.020] [out] [blockchain_module] \x1B[2m2026-08-21T05:05:27Z\x1B[0m "
            "\x1B[32m INFO\x1B[0m Service 'Tracing' is ready.");
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].level, QString("out"));
        QCOMPARE(lines[0].source, QString("blockchain_module"));
        QCOMPARE(lines[0].message,
                 QString("2026-08-21T05:05:27Z  INFO Service 'Tracing' is ready."));
        QVERIFY(lines[0].raw.contains(QChar(0x1B)));
    }

    void uiHostWrapperSplitsOnEscapedNewline()
    {
        const auto lines = LogLineParser::parse(
            "ui-host [ \"package_manager_ui\" ]: \"RemoteLogosObject: async callMethod timed out\\n"
            "[LogosObject] RemoteLogosObject::callMethodAsync \\\"getCatalog\\\" args: 0\"");
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[0].source, QString("package_manager_ui"));
        QCOMPARE(lines[0].level, LogLineNames::kLevelPlain);
        QCOMPARE(lines[0].message, QString("RemoteLogosObject: async callMethod timed out"));
        QCOMPARE(lines[1].message,
                 QString("[LogosObject] RemoteLogosObject::callMethodAsync \"getCatalog\" args: 0"));
        QCOMPARE(lines[0].raw, lines[1].raw);
        QCOMPARE(lines[0].continuesBundle, false);
        QCOMPARE(lines[1].continuesBundle, true);
    }

    void uiHostUnicodeEscapes()
    {
        // QDebug writes non-printables as \uXXXX and tabs as \t.
        const auto lines = LogLineParser::parse(
            "ui-host [ \"chat_ui\" ]: \"bell\\u0007 tab\\tend \\\\ back\"");
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].message, QString("bell tab\tend \\ back"));
        // A malformed escape is kept verbatim rather than dropped.
        const auto bad = LogLineParser::parse("ui-host [ \"chat_ui\" ]: \"x\\uZZ\"");
        QCOMPARE(bad.size(), 1);
        QCOMPARE(bad[0].message, QString("x\\uZZ"));
    }

    void uiHostQmlDiagnosticIsQmlLevel()
    {
        const auto lines = LogLineParser::parse(
            "ui-host [ \"storage_ui\" ]: \"qrc:/qt/qml/Main.qml:12:5: TypeError: Cannot read property 'x' of null\"");
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].level, LogLineNames::kLevelQml);
        QCOMPARE(lines[0].source, QString("storage_ui"));
    }

    void mainProcessQmlDiagnostic()
    {
        const auto lines = LogLineParser::parse(
            "qrc:/qt/qml/Basecamp/Shell/Basecamp/Shell/ContentViews.qml:48: TypeError: "
            "Cannot read property 'repositories' of null");
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].level, LogLineNames::kLevelQml);
        QCOMPARE(lines[0].source, LogLineNames::kSourceApp);
        QVERIFY(lines[0].timestamp.isEmpty());
    }

    void plainBasecampLine()
    {
        const auto lines = LogLineParser::parse("MainContainer: Active section index changed to 3");
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].level, LogLineNames::kLevelPlain);
        QCOMPARE(lines[0].source, LogLineNames::kSourceApp);
        QCOMPARE(lines[0].message, QString("MainContainer: Active section index changed to 3"));
    }

    void blankLinesDropped()
    {
        QVERIFY(LogLineParser::parse("").isEmpty());
        QVERIFY(LogLineParser::parse("   ").isEmpty());
        QVERIFY(LogLineParser::parse("\r").isEmpty());
    }

    void carriageReturnTrimmed()
    {
        const auto lines = LogLineParser::parse("hello\r");
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].message, QString("hello"));
    }

    void parseTextSplitsAndSkipsBlank()
    {
        const auto lines = LogLineParser::parseText(
            "a\n\n[2026-08-21 10:04:23.875] [info] [logos] [x] b\nc");
        QCOMPARE(lines.size(), 3);
        QCOMPARE(lines[0].message, QString("a"));
        QCOMPARE(lines[1].source, QString("x"));
        QCOMPARE(lines[2].message, QString("c"));
    }

    // Session file naming is shared between LogRedirector (writer) and the
    // viewer/retention (readers) via LogosBasecampPaths; round-trip it.
    void fileNames()
    {
        using namespace LogosBasecampPaths;
        QString stamp;
        int rotation = -1;
        QVERIFY(parseSessionLogFileName("basecamp_20260821_100423.log", &stamp, &rotation));
        QCOMPARE(stamp, QString("20260821_100423"));
        QCOMPARE(rotation, 0);

        QVERIFY(parseSessionLogFileName("basecamp_20260728_141524.140.log", &stamp, &rotation));
        QCOMPARE(stamp, QString("20260728_141524"));
        QCOMPARE(rotation, 140);

        // A long session rotates past 999: the writer widens the field and
        // the reader must still see those files.
        QVERIFY(parseSessionLogFileName("basecamp_20260728_141524.1645.log", &stamp, &rotation));
        QCOMPARE(rotation, 1645);

        QVERIFY(!parseSessionLogFileName("basecamp.log", nullptr, nullptr));
        QVERIFY(!parseSessionLogFileName("other_20260821_100423.log", nullptr, nullptr));
        QVERIFY(!parseSessionLogFileName("basecamp_20260821_100423.log.bak", nullptr, nullptr));

        for (int r : { 0, 1, 42, 999, 1000, 1645 }) {
            const QString name = sessionLogFileName("20260728_141524", r);
            QVERIFY2(parseSessionLogFileName(name, &stamp, &rotation), qPrintable(name));
            QCOMPARE(stamp, QString("20260728_141524"));
            QCOMPARE(rotation, r);
        }
        QCOMPARE(sessionLogFileName("s", 0), QString("basecamp_s.log"));
        QCOMPARE(sessionLogFileName("s", 7), QString("basecamp_s.007.log"));
        QCOMPARE(sessionLogFileName("s", 1000), QString("basecamp_s.1000.log"));
    }

    void stripAnsiLeavesPlainTextAlone()
    {
        QCOMPARE(LogLineParser::stripAnsi("plain"), QString("plain"));
        QCOMPARE(LogLineParser::stripAnsi("\x1B[1;31mred\x1B[0m"), QString("red"));
    }
};

QTEST_GUILESS_MAIN(LogLineParserTest)
#include "log_line_parser_test.moc"
