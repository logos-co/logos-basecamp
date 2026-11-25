#pragma once

#include <QByteArray>
#include <QString>

// A package's metadata.json, read as intents_module reads it: a regular file
// only, never blocking on open (a file a package left there may be a FIFO,
// which would hold the UI thread for good), and at most kMaxBytes (a larger
// one would be read whole, on that thread, at every rescan).
namespace logos::manifest {

inline constexpr qint64 kMaxBytes = 1024 * 1024;

enum class Read { Ok, Missing, NotRegular, TooLarge, Unreadable };

// `<dir>/metadata.json`; `bytes` holds it only on Ok.
Read read(const QString& dir, QByteArray* bytes);

// Why it wasn't read, for a diagnostic ("larger than 1 MiB").
QString describe(Read result);

} // namespace logos::manifest
