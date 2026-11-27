#include <QtTest/QtTest>

#include <QFile>
#include <QRegularExpression>

// Basecamp calls package_downloader through the typed wrappers
// (LogosModules(api).package_downloader.fooAsyncResult). The raw
// invokeRemoteMethodAsync hands a timeout over as an empty QVariant, which
// read as no repositories, no download source, or a source that wasn't saved.
class DownloaderCallsTest : public QObject {
    Q_OBJECT

    // The source without comments, so prose may still name the call.
    static QString codeOf(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
        QString text = QString::fromUtf8(file.readAll());
        text.remove(QRegularExpression(QStringLiteral(R"(/\*.*?\*/)"),
                                       QRegularExpression::DotMatchesEverythingOption));
        text.remove(QRegularExpression(QStringLiteral(R"(//[^\n]*)")));
        return text;
    }

private slots:
    void package_downloader_is_called_through_its_wrappers()
    {
        const QString code = codeOf(QStringLiteral(BASECAMP_SRC_DIR "/PackageCoordinator.cpp"));
        QVERIFY(code.contains(QStringLiteral("package_downloader.getDownloadSourceAsyncResult(")));

        const QRegularExpression raw(QStringLiteral(
            R"re(invokeRemoteMethod\w*\(\s*"package_downloader"\s*,\s*"(\w+)")re"));
        QStringList offenders;
        for (auto it = raw.globalMatch(code); it.hasNext();) offenders << it.next().captured(1);
        // getCatalog: logos-basecamp#572.
        offenders.removeAll(QStringLiteral("getCatalog"));
        QCOMPARE(offenders, QStringList());
    }
};

QTEST_GUILESS_MAIN(DownloaderCallsTest)
#include "downloader_calls_test.moc"
