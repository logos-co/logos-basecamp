#include "LogLine.h"

#include <QRegularExpression>
#include <QStringList>

namespace LogLineParser {

namespace {

// "[2026-08-21 10:04:23.875]" — what spdlog and logos_host put first.
constexpr const char* kTs = R"rx(\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)\])rx";

const QRegularExpression& ansiRe()
{
    static const QRegularExpression re(QStringLiteral("\x1B\\[[0-9;?]*[ -/]*[@-~]"));
    return re;
}

// [ts] [level] [logos] [module] message   /   [ts] [level] [logos] message
const QRegularExpression& logosRe()
{
    static const QRegularExpression re(
        QStringLiteral("^") + QString::fromLatin1(kTs)
        + QStringLiteral(R"rx( \[([A-Za-z]+)\] \[logos\](?: \[([A-Za-z0-9_.-]+)\])? ?(.*)$)rx"));
    return re;
}

// [ts] [out] [module] message — raw stdout of a module child.
const QRegularExpression& moduleRe()
{
    static const QRegularExpression re(
        QStringLiteral("^") + QString::fromLatin1(kTs)
        + QStringLiteral(R"rx( \[([A-Za-z]+)\] \[([A-Za-z0-9_.-]+)\] ?(.*)$)rx"));
    return re;
}

// ui-host [ "name" ]: "escaped text"
const QRegularExpression& uiHostRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"rx(^ui-host \[ "([^"]+)" \]: "(.*)"$)rx"));
    return re;
}

// qrc:/qt/qml/.../File.qml:48: TypeError: ...   file:///.../Main.qml:12:5: ...
const QRegularExpression& qmlRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"rx(^(?:qrc:|file:)\S+\.(?:qml|js):\d+(?::\d+)?:)rx"));
    return re;
}

// Undo the escaping QDebug applies when a QString is streamed: the relayed
// ui-host output arrives as one quoted string with \n, \" and \\ escaped.
QString unescapeQDebug(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c != QLatin1Char('\\') || i + 1 >= s.size()) {
            out.append(c);
            continue;
        }
        const QChar n = s.at(i + 1);
        switch (n.unicode()) {
        case 'n':  out.append(QLatin1Char('\n')); ++i; break;
        case 't':  out.append(QLatin1Char('\t')); ++i; break;
        case 'r':  ++i; break;
        case '"':  out.append(QLatin1Char('"'));  ++i; break;
        case '\\': out.append(QLatin1Char('\\')); ++i; break;
        case 'u': {
            // QDebug writes non-printable characters as \uXXXX.
            bool ok = false;
            const uint code = (i + 5 < s.size()) ? s.mid(i + 2, 4).toUInt(&ok, 16) : 0;
            if (ok) { out.append(QChar(code)); i += 5; }
            else    { out.append(c); }
            break;
        }
        default:   out.append(c); break;
        }
    }
    return out;
}

LogLine make(const QString& ts, const QString& level, const QString& source,
             const QString& message, const QString& raw)
{
    LogLine l;
    l.timestamp = ts;
    l.level = level.toLower();
    l.source = source;
    l.message = stripAnsi(message);
    l.raw = raw;
    return l;
}

} // namespace

QString stripAnsi(const QString& text)
{
    if (!text.contains(QChar(0x1B))) return text;
    QString out = text;
    out.remove(ansiRe());
    return out;
}

QList<LogLine> parse(const QString& rawLine)
{
    QList<LogLine> out;
    QString line = rawLine;
    if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
    if (line.trimmed().isEmpty()) return out;

    QRegularExpressionMatch m = logosRe().match(line);
    if (m.hasMatch()) {
        const QString module = m.captured(3);
        out.append(make(m.captured(1), m.captured(2),
                        module.isEmpty() ? LogLineNames::kSourceCore : module,
                        m.captured(4), line));
        return out;
    }

    m = moduleRe().match(line);
    if (m.hasMatch()) {
        out.append(make(m.captured(1), m.captured(2), m.captured(3), m.captured(4), line));
        return out;
    }

    m = uiHostRe().match(line);
    if (m.hasMatch()) {
        const QString plugin = m.captured(1);
        const QStringList pieces = unescapeQDebug(m.captured(2)).split(QLatin1Char('\n'));
        for (const QString& piece : pieces) {
            if (piece.trimmed().isEmpty()) continue;
            const bool isQml = qmlRe().match(piece).hasMatch();
            LogLine l = make(QString(),
                             isQml ? LogLineNames::kLevelQml : LogLineNames::kLevelPlain,
                             plugin, piece, line);
            l.continuesBundle = !out.isEmpty();
            out.append(l);
        }
        return out;
    }

    if (qmlRe().match(line).hasMatch()) {
        out.append(make(QString(), LogLineNames::kLevelQml, LogLineNames::kSourceApp, line, line));
        return out;
    }

    out.append(make(QString(), LogLineNames::kLevelPlain, LogLineNames::kSourceApp, line, line));
    return out;
}

QList<LogLine> parseText(const QString& text)
{
    QList<LogLine> out;
    int start = 0;
    const int n = text.size();
    while (start < n) {
        int end = text.indexOf(QLatin1Char('\n'), start);
        if (end < 0) end = n;
        out.append(parse(text.mid(start, end - start)));
        start = end + 1;
    }
    return out;
}

} // namespace LogLineParser
