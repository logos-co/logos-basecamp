// srcdeps: utils/ManifestFile.cpp
//
// A package's metadata.json, read without trusting what is on disk: only a
// regular file, never blocking on open, at most 1 MiB. Basecamp reads it on
// the UI thread, where a FIFO would hold the app for good and a huge file
// would be read whole at every rescan.

#include "utils/ManifestFile.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <future>
#include <thread>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using logos::manifest::Read;

namespace {

void write(const QString& dir, const QByteArray& bytes)
{
    QFile f(QDir(dir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(f.write(bytes), bytes.size());
}

} // namespace

class TestManifestFile : public QObject {
    Q_OBJECT

private slots:
    void aRegularFileIsRead();
    void noFileIsMissing();
    void aFifoIsNotReadAndDoesNotBlock();
    void aDeviceIsNotRead();
    void aDirectoryIsNotRead();
    void overOneMiBIsNotRead();
};

void TestManifestFile::aRegularFileIsRead()
{
    QTemporaryDir dir;
    const QByteArray text = R"({"name":"wallet","provides":[{"intent":"wallet.send"}]})";
    write(dir.path(), text);
    QByteArray bytes;
    QCOMPARE(logos::manifest::read(dir.path(), &bytes), Read::Ok);
    QCOMPARE(bytes, text);
}

void TestManifestFile::noFileIsMissing()
{
    QTemporaryDir dir;
    QByteArray bytes = "stale";
    QCOMPARE(logos::manifest::read(dir.path(), &bytes), Read::Missing);
    QVERIFY(bytes.isEmpty());
    QCOMPARE(logos::manifest::read(QString(), &bytes), Read::Missing);
    QCOMPARE(logos::manifest::read(dir.filePath(QStringLiteral("gone")), &bytes), Read::Missing);
}

// Opening a FIFO for reading waits for a writer that never comes.
void TestManifestFile::aFifoIsNotReadAndDoesNotBlock()
{
#ifdef Q_OS_WIN
    QSKIP("no FIFOs on Windows");
#else
    QTemporaryDir dir;
    const QByteArray fifo = QFile::encodeName(dir.filePath(QStringLiteral("metadata.json")));
    QCOMPARE(::mkfifo(fifo.constData(), 0600), 0);
    std::promise<Read> result;
    std::future<Read> done = result.get_future();
    std::thread reader([&]() {
        QByteArray bytes;
        result.set_value(logos::manifest::read(dir.path(), &bytes));
    });
    const bool returned = done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!returned) {
        // Let the stuck read go, so the failure doesn't hang the suite.
        const int fd = ::open(fifo.constData(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0) ::close(fd);
    }
    reader.join();
    QVERIFY2(returned, "reading a FIFO blocked");
    QCOMPARE(done.get(), Read::NotRegular);
#endif
}

void TestManifestFile::aDeviceIsNotRead()
{
#ifdef Q_OS_WIN
    QSKIP("no /dev/zero on Windows");
#else
    QTemporaryDir dir;
    QVERIFY(QFile::link(QStringLiteral("/dev/zero"), dir.filePath(QStringLiteral("metadata.json"))));
    QByteArray bytes;
    QCOMPARE(logos::manifest::read(dir.path(), &bytes), Read::NotRegular);
    QVERIFY(bytes.isEmpty());
#endif
}

void TestManifestFile::aDirectoryIsNotRead()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("metadata.json")));
    QByteArray bytes;
    QCOMPARE(logos::manifest::read(dir.path(), &bytes), Read::NotRegular);
}

void TestManifestFile::overOneMiBIsNotRead()
{
    QTemporaryDir dir;
    QByteArray bytes;
    write(dir.path(), QByteArray(logos::manifest::kMaxBytes, ' '));
    QCOMPARE(logos::manifest::read(dir.path(), &bytes), Read::Ok);
    QCOMPARE(bytes.size(), logos::manifest::kMaxBytes);
    write(dir.path(), QByteArray(logos::manifest::kMaxBytes + 1, ' '));
    QCOMPARE(logos::manifest::read(dir.path(), &bytes), Read::TooLarge);
    QVERIFY(bytes.isEmpty());
    QCOMPARE(logos::manifest::describe(Read::TooLarge), QStringLiteral("larger than 1 MiB"));
}

QTEST_APPLESS_MAIN(TestManifestFile)
#include "manifest_file_test.moc"
