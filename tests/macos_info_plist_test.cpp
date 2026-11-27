//
// Unit tests for the macOS bundle's Info.plist template (app/macos/Info.plist.in),
// which both macOS bundles use (flake.nix). macOS ends a process at its first
// Bluetooth or local-network use when the bundle declares no usage text for it,
// and modules run as Basecamp's processes, so only Basecamp can declare it (#527).
//
// Run: nix build .#unit-tests -L

#include <QtTest/QtTest>
#include <QXmlStreamReader>

class MacosInfoPlistTest : public QObject {
    Q_OBJECT

    // Top-level <key> → <string> pairs of the plist's dict.
    static QMap<QString, QString> strings(const QString& path, QString* error)
    {
        QMap<QString, QString> out;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("cannot open ") + path;
            return out;
        }
        QXmlStreamReader xml(&file);
        int depth = 0;
        QString key;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                ++depth;
                // plist > dict > key/string
                if (depth == 3 && xml.name() == QLatin1String("key")) {
                    key = xml.readElementText();
                    --depth;
                } else if (depth == 3 && xml.name() == QLatin1String("string") && !key.isEmpty()) {
                    out.insert(key, xml.readElementText());
                    --depth;
                    key.clear();
                } else if (depth == 3) {
                    key.clear();
                }
            } else if (xml.isEndElement()) {
                --depth;
            }
        }
        if (xml.hasError()) *error = xml.errorString();
        return out;
    }

private slots:
    void declares_the_usage_texts_modules_need_data()
    {
        QTest::addColumn<QString>("key");
        QTest::newRow("Bluetooth") << QStringLiteral("NSBluetoothAlwaysUsageDescription");
        QTest::newRow("local network") << QStringLiteral("NSLocalNetworkUsageDescription");
    }
    void declares_the_usage_texts_modules_need()
    {
        QFETCH(QString, key);
        const QString path = QFINDTESTDATA("../app/macos/Info.plist.in");
        QVERIFY2(!path.isEmpty(), "app/macos/Info.plist.in not found");
        QString error;
        const QMap<QString, QString> plist = strings(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(plist.contains(QStringLiteral("CFBundleExecutable")), "plist not parsed");
        QVERIFY2(!plist.value(key).trimmed().isEmpty(), qPrintable(key + " is missing or empty"));
    }
};

QTEST_GUILESS_MAIN(MacosInfoPlistTest)
#include "macos_info_plist_test.moc"
