#pragma once

#include <QList>
#include <QMetaType>
#include <QString>

// One parsed line of a Basecamp session log.
//
// Everything Basecamp and its child processes print ends up in one file per
// session (see app/utils/LogRedirector). Four line shapes show up there, and
// the parser normalises all of them onto this struct so the Settings → Logs
// view can filter by origin and severity without knowing who wrote what:
//
//   [ts] [level] [logos] [module] msg   core module via spdlog in logos_host
//   [ts] [level] [logos] msg            liblogos itself (no module tag)
//   [ts] [out] [module] msg             a module's raw stdout, relayed by logos_host
//   ui-host [ "name" ]: "msg\nmsg"      a UI plugin's output, relayed by ui-host
//                                       (several lines folded into one, '\n' escaped)
//   qrc:/... :NN: TypeError ...         QML engine diagnostics (Basecamp's own engine)
//   anything else                       Basecamp's own qDebug/qInfo, no prefix
struct LogLine {
    QString timestamp;  // as written, e.g. "2026-08-21 10:04:23.875"; empty when absent
    QString level;      // info | warning | critical | debug | trace | out | qml | plain
    QString source;     // basecamp | liblogos | <module name> | <ui plugin name>
    QString message;    // ANSI-stripped text after the prefix
    QString raw;        // the physical line as read (ui-host pieces share one raw)
    // True for the 2nd.. pieces of a ui-host bundle: this row and the one
    // before it came from the same physical line. Lets a copy emit that line
    // once without mistaking two genuinely identical lines for one.
    bool continuesBundle = false;

    bool operator==(const LogLine& o) const
    {
        return timestamp == o.timestamp && level == o.level && source == o.source
            && message == o.message && raw == o.raw && continuesBundle == o.continuesBundle;
    }
};

Q_DECLARE_METATYPE(LogLine)
Q_DECLARE_METATYPE(QList<LogLine>)

// Names used for the two synthetic levels and the two fixed sources, so the
// QML side and tests don't repeat the literals.
namespace LogLineNames {
inline const QString kLevelPlain  = QStringLiteral("plain");
inline const QString kLevelQml    = QStringLiteral("qml");
inline const QString kLevelOut    = QStringLiteral("out");
inline const QString kSourceApp   = QStringLiteral("basecamp");
inline const QString kSourceCore  = QStringLiteral("liblogos");
}

namespace LogLineParser {

// Remove ANSI SGR / CSI escape sequences (colour codes from Rust tracing,
// Nim chronicles, spdlog) so messages render as plain text.
QString stripAnsi(const QString& text);

// Parse one physical line. Usually returns one LogLine; a ui-host wrapper
// returns one per embedded '\n'; a blank line returns none.
QList<LogLine> parse(const QString& rawLine);

// Parse a whole buffer split on '\n'. A trailing '\r' is dropped per line.
QList<LogLine> parseText(const QString& text);

// File names are LogosBasecampPaths::parseSessionLogFileName() — the same
// definition the writer uses.

} // namespace LogLineParser
