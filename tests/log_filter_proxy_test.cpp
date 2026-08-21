// srcdeps: LogFilterProxy.cpp LogLinesModel.cpp
//
// Unit tests for LogLinesModel (role contract, append/reset tallies) and
// LogFilterProxy (level / source / text filters, counts). Same harness as
// modules_filter_proxy_test.

#include "LogFilterProxy.h"
#include "LogLinesModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

LogLine line(const QString& level, const QString& source, const QString& message,
             const QString& ts = QStringLiteral("2026-08-21 10:00:00.000"))
{
    LogLine l;
    l.timestamp = ts;
    l.level = level;
    l.source = source;
    l.message = message;
    l.raw = message;
    return l;
}

QList<LogLine> sample()
{
    return {
        line("info",     "package_downloader", "callRemoteMethod listRepositories"),
        line("info",     "package_manager",    "callRemoteMethod getInstalledPackages"),
        line("warning",  "liblogos",           "Process did not terminate gracefully"),
        line("plain",    "basecamp",           "MainContainer: Active section index changed", ""),
        line("out",      "blockchain_module",  "Service 'Tracing' is ready."),
        line("plain",    "package_manager_ui", "RemoteLogosObject: async callMethod timed out", ""),
    };
}

QStringList sourcesOf(const LogFilterProxy& p)
{
    QStringList out;
    for (int i = 0; i < p.rowCount(); ++i)
        out << p.data(p.index(i, 0), LogLinesModel::SourceRole).toString();
    return out;
}

} // namespace

class LogFilterProxyTest : public QObject {
    Q_OBJECT

private slots:
    void roleNames_stable()
    {
        LogLinesModel model;
        const auto roles = model.roleNames();
        for (const QByteArray& name : { "timestamp", "level", "source", "message", "raw", "lineNumber" })
            QVERIFY2(roles.values().contains(name), name.constData());
    }

    void resetAndAppendKeepOrderAndCounts()
    {
        LogLinesModel model;
        QSignalSpy counts(&model, &LogLinesModel::countsChanged);

        model.reset(sample());
        QCOMPARE(model.count(), 6);
        QCOMPARE(counts.count(), 1);
        QCOMPARE(model.data(model.index(0), LogLinesModel::LineNumberRole).toInt(), 1);
        QCOMPARE(model.data(model.index(5), LogLinesModel::LineNumberRole).toInt(), 6);

        model.append({ line("critical", "package_downloader", "fetch failed") });
        QCOMPARE(model.count(), 7);
        QCOMPARE(counts.count(), 2);

        // Source tallies: basecamp pinned first, then by count desc, then name.
        const QVariantList sources = model.sourceCounts();
        QCOMPARE(sources.first().toMap().value("name").toString(), QString("basecamp"));
        QCOMPARE(sources.at(1).toMap().value("name").toString(), QString("package_downloader"));
        QCOMPARE(sources.at(1).toMap().value("count").toInt(), 2);

        const QVariantList levels = model.levelCounts();
        QCOMPARE(levels.first().toMap().value("name").toString(), QString("info"));
        QCOMPARE(levels.first().toMap().value("count").toInt(), 2);

        model.clear();
        QCOMPARE(model.count(), 0);
        QVERIFY(model.sourceCounts().isEmpty());
    }

    void noFilterPassesEverythingInOrder()
    {
        LogLinesModel model;
        model.reset(sample());
        LogFilterProxy proxy;
        proxy.setSourceModel(&model);

        QCOMPARE(proxy.totalCount(), 6);
        QCOMPARE(proxy.visibleCount(), 6);
        QCOMPARE(sourcesOf(proxy).first(), QString("package_downloader"));
        QCOMPARE(proxy.sourceRow(3), 3);
    }

    void levelFilter()
    {
        LogLinesModel model;
        model.reset(sample());
        LogFilterProxy proxy;
        proxy.setSourceModel(&model);
        QSignalSpy visible(&proxy, &LogFilterProxy::visibleCountChanged);

        proxy.setLevels({ "warning", "out" });
        QCOMPARE(proxy.visibleCount(), 2);
        QCOMPARE(sourcesOf(proxy), QStringList({ "liblogos", "blockchain_module" }));
        QVERIFY(visible.count() >= 1);

        proxy.setLevels({});
        QCOMPARE(proxy.visibleCount(), 6);
    }

    void sourceFilter()
    {
        LogLinesModel model;
        model.reset(sample());
        LogFilterProxy proxy;
        proxy.setSourceModel(&model);

        proxy.setSources({ "package_manager_ui" });
        QCOMPARE(proxy.visibleCount(), 1);
        QCOMPARE(proxy.sourceRow(0), 5);

        proxy.setSources({ "package_manager_ui", "basecamp" });
        QCOMPARE(proxy.visibleCount(), 2);
    }

    void searchIsCaseInsensitiveOverMessageSourceTimestamp()
    {
        LogLinesModel model;
        model.reset(sample());
        LogFilterProxy proxy;
        proxy.setSourceModel(&model);

        proxy.setSearchText("TIMED OUT");
        QCOMPARE(proxy.visibleCount(), 1);

        proxy.setSearchText("  package_ ");
        QCOMPARE(proxy.visibleCount(), 3);   // two module sources + the ui plugin

        proxy.setSearchText("10:00:00");
        QCOMPARE(proxy.visibleCount(), 4);   // the two timestamp-less rows drop out

        proxy.setSearchText("");
        QCOMPARE(proxy.visibleCount(), 6);
    }

    void filtersCombine()
    {
        LogLinesModel model;
        model.reset(sample());
        LogFilterProxy proxy;
        proxy.setSourceModel(&model);

        proxy.setLevels({ "info" });
        proxy.setSources({ "package_downloader", "package_manager" });
        proxy.setSearchText("installed");
        QCOMPARE(proxy.visibleCount(), 1);
        QCOMPARE(sourcesOf(proxy), QStringList({ "package_manager" }));
    }

    void textForRowsJoinsPhysicalLinesOnce()
    {
        LogLinesModel model;
        // Two display rows that came from one ui-host bundle share `raw`; the
        // second is marked as continuing the first.
        LogLine a = line("plain", "storage_ui", "first piece");
        LogLine b = line("plain", "storage_ui", "second piece");
        a.raw = b.raw = QStringLiteral("ui-host [ \"storage_ui\" ]: \"first piece\\nsecond piece\"");
        b.continuesBundle = true;
        // Two genuinely separate, identical physical lines.
        const LogLine twin = line("info", "x", "same again");
        model.reset({ line("info", "x", "one"), a, b, line("info", "x", "four"), twin, twin });
        LogFilterProxy proxy;
        proxy.setSourceModel(&model);

        QCOMPARE(proxy.lineCount(0, 3), 3);
        QCOMPARE(proxy.textForRows(0, 3), QString("one\n%1\nfour").arg(a.raw));
        // Either order, clamped to the model.
        QCOMPARE(proxy.textForRows(3, 0), proxy.textForRows(0, 3));
        QCOMPARE(proxy.textForRows(-5, 3), proxy.textForRows(0, 3));
        // A range inside the bundle yields the bundle line once.
        QCOMPARE(proxy.textForRows(1, 2), a.raw);
        QCOMPARE(proxy.lineCount(1, 2), 1);
        // Identical neighbours that are separate lines stay separate.
        QCOMPARE(proxy.lineCount(4, 5), 2);
        QCOMPARE(proxy.textForRows(4, 5), QString("same again\nsame again"));
        // maxLines truncates.
        QCOMPARE(proxy.textForRows(0, 3, 2), QString("one\n%1").arg(a.raw));
        // Filtering applies: rows are proxy rows.
        proxy.setSources({ "x" });
        QCOMPARE(proxy.textForRows(0, 1), QString("one\nfour"));
    }

    void appendsFlowThroughLiveFilter()
    {
        LogLinesModel model;
        model.reset(sample());
        LogFilterProxy proxy;
        proxy.setSourceModel(&model);
        proxy.setSources({ "package_downloader" });
        QCOMPARE(proxy.visibleCount(), 1);

        model.append({ line("critical", "package_downloader", "fetch failed"),
                       line("info", "other", "ignored") });
        QCOMPARE(proxy.visibleCount(), 2);
        QCOMPARE(proxy.totalCount(), 8);
    }
};

QTEST_GUILESS_MAIN(LogFilterProxyTest)
#include "log_filter_proxy_test.moc"
