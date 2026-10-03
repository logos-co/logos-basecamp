// srcdeps: utils/ModuleConfigOption.cpp
//
// Unit tests for Basecamp's --module-config / LOGOS_MODULE_CONFIG resolution
// (app/utils/ModuleConfigOption.{h,cpp}): each module's configuration, which
// the runtime hands the module as it starts. Absent means none, and anything
// that cannot be honoured stops the app rather than starting modules without
// what was asked for. Run: nix build .#unit-tests -L

#include "../app/utils/ModuleConfigOption.h"

#include <QtTest/QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using LogosBasecamp::resolveModuleConfig;

class ModuleConfigOptionTest : public QObject
{
    Q_OBJECT

private slots:
    void noArgumentConfiguresNothing()
    {
        for (const QString& arg : {QString(), QStringLiteral("  ")}) {
            const auto r = resolveModuleConfig(arg);
            QVERIFY2(r.ok, qPrintable(r.error));
            QVERIFY(r.configJson.isEmpty());
        }
    }

    void inlineJsonIsTakenAsTheDocument()
    {
        // No "//" in these literals: moc reads it as a comment inside a raw string.
        const auto r = resolveModuleConfig(
            QStringLiteral(R"({"my_module":{"endpoint":"example.org:443"},"other":[1,2]})"));
        QVERIFY2(r.ok, qPrintable(r.error));
        const QJsonObject doc = QJsonDocument::fromJson(r.configJson.toUtf8()).object();
        QCOMPARE(doc.value(QStringLiteral("my_module")).toObject()
                     .value(QStringLiteral("endpoint")).toString(),
                 QStringLiteral("example.org:443"));
        QVERIFY(doc.value(QStringLiteral("other")).isArray());
    }

    void aFileIsRead()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("modules.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"my_module":{"retries":3}})");
        f.close();
        const auto r = resolveModuleConfig(path);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(QJsonDocument::fromJson(r.configJson.toUtf8()).object()
                     .value(QStringLiteral("my_module")).toObject()
                     .value(QStringLiteral("retries")).toInt(),
                 3);
    }

    // Detector: anything unusable must stop the app, never configure nothing.
    void anythingElseIsRefused()
    {
        for (const QString& bad : {QStringLiteral("{not json"), QStringLiteral("{\"a b\":{}}"),
                                   QStringLiteral("{\"\":{}}"),
                                   QStringLiteral("/definitely/not/a/file.json")}) {
            const auto r = resolveModuleConfig(bad);
            QVERIFY2(!r.ok, qPrintable(bad));
            QVERIFY2(!r.error.isEmpty(), qPrintable(bad));
            QVERIFY(r.configJson.isEmpty());
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("list.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("[1, 2]");
        f.close();
        QVERIFY(!resolveModuleConfig(path).ok);
    }
};

QTEST_GUILESS_MAIN(ModuleConfigOptionTest)
#include "module_config_option_test.moc"
