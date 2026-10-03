// srcdeps: utils/PeeringSettings.cpp
//
// Settings -> Peering's file (app/utils/PeeringSettings.{h,cpp}): peering is
// off unless peering.json says otherwise, and turning it on or off keeps the
// rest of the file. Run: nix build .#unit-tests -L

#include "../app/utils/PeeringSettings.h"

#include <QtTest/QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace LogosBasecamp;

namespace {

void write(const QString& path, const QByteArray& text)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(text);
}

QJsonObject configOf(const std::optional<std::string>& config)
{
    return config ? QJsonDocument::fromJson(QByteArray::fromStdString(*config)).object() : QJsonObject();
}

} // namespace

class PeeringSettingsTest : public QObject
{
    Q_OBJECT

private slots:
    void withoutAFilePeeringIsOff()
    {
        QTemporaryDir dir;
        const auto settings = loadPeeringSettings(peeringSettingsPath(dir.path()));
        QVERIFY(!settings.enabled);
        QVERIFY(settings.error.isEmpty());
        QVERIFY(!peeringRuntimeConfig(settings, QStringLiteral("Basecamp")));
    }

    void anEnabledFileBecomesTheRuntimesConfig()
    {
        QTemporaryDir dir;
        const QString path = peeringSettingsPath(dir.path());
        write(path, R"({"enabled": true, "control": {"enabled": false}})");
        const auto settings = loadPeeringSettings(path);
        QVERIFY(settings.enabled);
        const QJsonObject config = configOf(peeringRuntimeConfig(settings, QStringLiteral("Basecamp on desk")));
        QVERIFY(!config.contains(QStringLiteral("enabled")));
        QCOMPARE(config.value(QStringLiteral("name")).toString(), QStringLiteral("Basecamp on desk"));
        QVERIFY(config.value(QStringLiteral("control")).isObject());

        write(path, R"({"enabled": true, "name": "studio"})");
        QCOMPARE(configOf(peeringRuntimeConfig(loadPeeringSettings(path), QStringLiteral("x")))
                     .value(QStringLiteral("name")).toString(),
                 QStringLiteral("studio"));
    }

    void anUnreadableFileKeepsPeeringOff()
    {
        QTemporaryDir dir;
        const QString path = peeringSettingsPath(dir.path());
        write(path, "{\"enabled\": true,");
        const auto settings = loadPeeringSettings(path);
        QVERIFY(!settings.error.isEmpty());
        QVERIFY(!peeringRuntimeConfig(settings, QStringLiteral("Basecamp")));
        QString error;
        QVERIFY(!setPeeringEnabled(path, true, &error));
        QVERIFY(!error.isEmpty());
    }

    void enablingKeepsTheRestOfTheFile()
    {
        QTemporaryDir dir;
        const QString path = peeringSettingsPath(dir.path());
        QVERIFY(setPeeringEnabled(path, true));
        QVERIFY(loadPeeringSettings(path).enabled);
        write(path, R"({"enabled": true, "exports": {"enabled": true}})");
        QVERIFY(setPeeringEnabled(path, false));
        const auto settings = loadPeeringSettings(path);
        QVERIFY(!settings.enabled);
        QVERIFY(settings.config.value(QStringLiteral("exports")).isObject());
    }

    void namesAndPaths()
    {
        QCOMPARE(defaultPeeringName(QStringLiteral("studio.local")), QStringLiteral("Basecamp on studio"));
        QCOMPARE(defaultPeeringName(QString()), QStringLiteral("Basecamp"));
        QCOMPARE(defaultPeeringName(QString(100, QLatin1Char('h'))).size(), 64);
        QCOMPARE(daemonLocalInvitePath(QString(), QStringLiteral("/home/me")),
                 QStringLiteral("/home/me/.logosctl/peering/local-invite"));
        QCOMPARE(daemonLocalInvitePath(QStringLiteral("/srv/node"), QStringLiteral("/home/me")),
                 QStringLiteral("/srv/node/peering/local-invite"));
    }
};

QTEST_GUILESS_MAIN(PeeringSettingsTest)
#include "peering_settings_test.moc"
