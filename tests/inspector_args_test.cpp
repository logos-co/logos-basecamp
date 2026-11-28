// srcdeps: InspectorArgs.cpp ModuleCallFailure.cpp
#include <QtTest/QtTest>

#include <QJsonDocument>
#include <QJsonObject>

#include "InspectorArgs.h"

// basecamp#562: the Module Inspector called every method with no arguments,
// so a method that takes any found no match and every call read the same, and
// it refused more than 3 arguments and showed a failed call as a null result.
class InspectorArgsTest : public QObject {
    Q_OBJECT

    static QJsonArray params(std::initializer_list<std::pair<const char*, const char*>> list)
    {
        QJsonArray out;
        for (const auto& [name, type] : list)
            out.append(QJsonObject{{"name", name}, {"type", type}});
        return out;
    }

private slots:
    void each_text_is_read_as_its_type()
    {
        const InspectorArgs a = inspectorArgs(
            params({{"path", "QString"}, {"from", "qulonglong"}, {"delta", "qlonglong"},
                    {"on", "bool"}, {"ratio", "double"}, {"names", "QStringList"},
                    {"opts", "QVariantMap"}, {"items", "QVariantList"}}),
            {"/tmp/a b", "12", "-3", "True", "0.5", "[\"x\", \"y\"]", "{\"k\": 1}", "[1, \"two\"]"});
        QVERIFY2(a.error.isEmpty(), qPrintable(a.error));
        QCOMPARE(a.values.size(), 8);
        QCOMPARE(a.values[0], QVariant(QStringLiteral("/tmp/a b")));
        QCOMPARE(a.values[1].typeId(), QMetaType::ULongLong);
        QCOMPARE(a.values[1].toULongLong(), 12ULL);
        QCOMPARE(a.values[2].typeId(), QMetaType::LongLong);
        QCOMPARE(a.values[2].toLongLong(), -3LL);
        QCOMPARE(a.values[3], QVariant(true));
        QCOMPARE(a.values[4], QVariant(0.5));
        QCOMPARE(a.values[5].toStringList(), (QStringList{"x", "y"}));
        QCOMPARE(a.values[6].toMap().value("k").toInt(), 1);
        QCOMPARE(a.values[7].toList().size(), 2);
    }

    void a_text_keeps_its_spaces()
    {
        const InspectorArgs a = inspectorArgs(params({{"s", "QString"}}), {"  padded  "});
        QCOMPARE(a.values.value(0).toString(), QStringLiteral("  padded  "));
    }

    void no_parameters_is_no_arguments()
    {
        const InspectorArgs a = inspectorArgs(QJsonArray(), {});
        QVERIFY(a.error.isEmpty());
        QVERIFY(a.values.isEmpty());
    }

    void what_cannot_be_read_says_which_and_why()
    {
        QCOMPARE(inspectorArgs(params({{"from_slot", "qulonglong"}}), {"-1"}).error,
                 QStringLiteral("from_slot: expected a whole number, 0 or more"));
        QCOMPARE(inspectorArgs(params({{"n", "int"}}), {"ten"}).error,
                 QStringLiteral("n: expected a whole number"));
        QCOMPARE(inspectorArgs(params({{"on", "bool"}}), {"yes"}).error,
                 QStringLiteral("on: expected true or false"));
        QCOMPARE(inspectorArgs(params({{"addrs", "QStringList"}}), {"[1]"}).error,
                 QStringLiteral("addrs: expected a JSON list of strings, like [\"a\", \"b\"]"));
        QCOMPARE(inspectorArgs(params({{"m", "QVariantMap"}}), {"[]"}).error,
                 QStringLiteral("m: expected a JSON object, like {\"key\": 1}"));
        const InspectorArgs bad = inspectorArgs(params({{"a", "QString"}, {"b", "int"}}), {"x", "y"});
        QVERIFY(bad.values.isEmpty());
    }

    void a_count_mismatch_is_refused()
    {
        QCOMPARE(inspectorArgs(params({{"a", "QString"}}), {}).error,
                 QStringLiteral("expected 1 arguments, got 0"));
    }

    void an_unnamed_parameter_is_numbered()
    {
        QJsonArray p{QJsonObject{{"type", "int"}}};
        QCOMPARE(inspectorArgs(p, {"x"}).error, QStringLiteral("argument 1: expected a whole number"));
    }

    void a_variant_takes_json_or_text()
    {
        const InspectorArgs a = inspectorArgs(params({{"a", "QVariant"}, {"b", "QVariant"}}),
                                              {"42", "hello"});
        QCOMPARE(a.values[0].toLongLong(), 42LL);
        QCOMPARE(a.values[1], QVariant(QStringLiteral("hello")));
    }

    // The call itself, with a fake module that records what reached it.
    void every_argument_reaches_the_module_as_its_type()
    {
        const QJsonArray p = params({{"user_config_path", "QString"}, {"keystore_path", "QString"},
                                     {"key_type", "QString"}, {"key_hex", "QString"},
                                     {"key_title", "QString"}});
        QVariantList received;
        const QString payload = inspectorCall(
            "blockchain_module", QString::fromUtf8(QJsonDocument(p).toJson()),
            R"(["/c/user.yaml", "/c/keys", "ed25519", "ab12", "main"])", 20000,
            [&](const QVariantList& args) {
                received = args;
                return InspectorReply{QJsonObject{{"success", true}}, {}, {}};
            });
        QCOMPARE(received, (QVariantList{"/c/user.yaml", "/c/keys", "ed25519", "ab12", "main"}));
        QCOMPARE(payload, QStringLiteral(R"({"result":{"success":true}})"));

        const QJsonArray range = params({{"from_slot", "qulonglong"}, {"to_slot", "qulonglong"}});
        inspectorCall("blockchain_module", QString::fromUtf8(QJsonDocument(range).toJson()),
                      R"(["10", "20"])", 20000, [&](const QVariantList& args) {
            received = args;
            return InspectorReply{};
        });
        QCOMPARE(received.size(), 2);
        QCOMPARE(received[0].typeId(), QMetaType::ULongLong);
        QCOMPARE(received[1].toULongLong(), 20ULL);
    }

    void nothing_is_called_when_an_argument_cant_be_read()
    {
        bool called = false;
        const QString payload = inspectorCall(
            "blockchain_module", R"([{"name": "from_slot", "type": "qulonglong"}])",
            R"(["ten"])", 20000, [&](const QVariantList&) {
                called = true;
                return InspectorReply{};
            });
        QVERIFY(!called);
        QCOMPARE(payload,
                 QStringLiteral(R"({"error":"from_slot: expected a whole number, 0 or more"})"));
    }

    void a_failed_call_shows_its_error()
    {
        const QString payload = inspectorCall(
            "blockchain_module", "[]", "[]", 20000, [](const QVariantList&) {
                return InspectorReply{{}, "timeout", "call to 'x' timed out after 20000ms"};
            });
        QCOMPARE(payload,
                 QStringLiteral(R"({"error":"blockchain_module did not answer within 20 s."})"));
    }
};

QTEST_GUILESS_MAIN(InspectorArgsTest)
#include "inspector_args_test.moc"
