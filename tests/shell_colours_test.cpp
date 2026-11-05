// The shell draws every colour from the theme: Theme.palette in QML,
// ThemeColours (Logos::ThemeStore) in C++. A colour literal in src/ ignores
// custom themes (logos-basecamp#546), so this fails on any.

#include <QtTest/QtTest>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

class ShellColoursTest : public QObject
{
    Q_OBJECT

private slots:
    void shell_sources_have_no_colour_literals()
    {
        const QString shellDir =
            QFileInfo(QStringLiteral(__FILE__)).absoluteDir().filePath(QStringLiteral("../src"));
        QVERIFY2(QFileInfo(shellDir).isDir(), qPrintable(shellDir));

        // "#rgb", "#rrggbb", "#aarrggbb" and numeric rgb()/rgba().
        const QRegularExpression literal(QStringLiteral(
            R"((#[0-9a-fA-F]{8}\b|#[0-9a-fA-F]{6}\b|"#[0-9a-fA-F]{3}"|rgba?\(\s*\d))"));

        QStringList found;
        int files = 0;
        QDirIterator it(shellDir, {QStringLiteral("*.qml"), QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(path));
            ++files;
            int lineNo = 0;
            while (!f.atEnd()) {
                const QString line = QString::fromUtf8(f.readLine());
                ++lineNo;
                if (literal.match(line).hasMatch())
                    found << QStringLiteral("%1:%2: %3").arg(path).arg(lineNo).arg(line.trimmed());
            }
        }
        QVERIFY2(files > 20, "found too few shell sources to trust the scan");
        QVERIFY2(found.isEmpty(), qPrintable(QStringLiteral("use Theme.palette / ThemeColours:\n")
                                             + found.join(QLatin1Char('\n'))));
    }
};

QTEST_GUILESS_MAIN(ShellColoursTest)
#include "shell_colours_test.moc"
