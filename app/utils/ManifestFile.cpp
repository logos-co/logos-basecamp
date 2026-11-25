#include "utils/ManifestFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <utility>

#ifndef Q_OS_WIN
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace logos::manifest {

Read read(const QString& dir, QByteArray* bytes)
{
    bytes->clear();
    if (dir.isEmpty()) return Read::Missing;
    const QString path = QDir(dir).filePath(QStringLiteral("metadata.json"));
    QByteArray data;
#ifdef Q_OS_WIN
    const QFileInfo info(path);
    if (!info.exists()) return Read::Missing;
    if (!info.isFile()) return Read::NotRegular;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Read::Unreadable;
    data = file.read(kMaxBytes + 1);
#else
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? Read::Missing : Read::Unreadable;
    struct stat st {};
    const bool statted = ::fstat(fd, &st) == 0;
    if (!statted || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return statted ? Read::NotRegular : Read::Unreadable;
    }
    char buffer[64 * 1024];
    bool failed = false;
    while (data.size() <= kMaxBytes) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            failed = n < 0;
            break;
        }
        data.append(buffer, n);
    }
    ::close(fd);
    if (failed) return Read::Unreadable;
#endif
    if (data.size() > kMaxBytes) return Read::TooLarge;
    *bytes = std::move(data);
    return Read::Ok;
}

QString describe(Read result)
{
    switch (result) {
    case Read::Ok:         return {};
    case Read::Missing:    return QStringLiteral("not there");
    case Read::NotRegular: return QStringLiteral("not a regular file");
    case Read::TooLarge:   return QStringLiteral("larger than 1 MiB");
    case Read::Unreadable: return QStringLiteral("unreadable");
    }
    return {};
}

} // namespace logos::manifest
