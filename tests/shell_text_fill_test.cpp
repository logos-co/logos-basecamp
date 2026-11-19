// A sentence filled by chained .arg() substitutes again into what an earlier
// .arg() put in, so a name that reads "%2" takes the next value and rewrites
// the sentence around it. Shell QML fills in one pass (a fill() helper), so
// this fails on any chain.

#include <QtTest/QtTest>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

class ShellTextFillTest : public QObject
{
    Q_OBJECT

    // Where the parenthesis opened at `open` closes, or -1.
    static qsizetype closing(const QString& text, qsizetype open)
    {
        int depth = 0;
        for (qsizetype i = open; i < text.size(); ++i) {
            if (text.at(i) == QLatin1Char('(')) ++depth;
            else if (text.at(i) == QLatin1Char(')') && --depth == 0) return i;
        }
        return -1;
    }

private slots:
    void shell_sentences_are_filled_in_one_pass()
    {
        const QString shellDir =
            QFileInfo(QStringLiteral(__FILE__)).absoluteDir().filePath(QStringLiteral("../src"));
        QVERIFY2(QFileInfo(shellDir).isDir(), qPrintable(shellDir));

        const QString arg = QStringLiteral(".arg(");
        QStringList found;
        int files = 0;
        QDirIterator it(shellDir, {QStringLiteral("*.qml"), QStringLiteral("*.js")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(path));
            ++files;
            const QString text = QString::fromUtf8(f.readAll());
            for (qsizetype at = text.indexOf(arg); at >= 0; at = text.indexOf(arg, at + 1)) {
                const qsizetype close = closing(text, at + arg.size() - 1);
                if (close < 0) break;
                qsizetype next = close + 1;
                while (next < text.size() && text.at(next).isSpace()) ++next;
                if (QStringView(text).mid(next, arg.size()) == arg)
                    found << QStringLiteral("%1:%2").arg(path).arg(text.left(at).count(QLatin1Char('\n')) + 1);
            }
        }
        QVERIFY2(files > 20, "found too few shell sources to trust the scan");
        QVERIFY2(found.isEmpty(), qPrintable(QStringLiteral("fill in one pass, not with chained .arg():\n")
                                             + found.join(QLatin1Char('\n'))));
    }
};

QTEST_GUILESS_MAIN(ShellTextFillTest)
#include "shell_text_fill_test.moc"
