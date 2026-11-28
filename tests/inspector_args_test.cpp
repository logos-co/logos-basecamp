// srcdeps: InspectorArgs.cpp
#include <QtTest/QtTest>

#include <QJsonObject>

#include "InspectorArgs.h"

// basecamp#562: the Module Inspector called every method with no arguments,
// so a method that takes any found no match and every call read the same.
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
};

QTEST_GUILESS_MAIN(InspectorArgsTest)
#include "inspector_args_test.moc"
