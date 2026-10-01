#include <QtTest>
#include "OptionalDependencyPreview.h"

class OptionalDependencyPreviewTest : public QObject {
    Q_OBJECT
    static QVariantMap offer(QString name, QString parent, QString version = "1.0.0") {
        return {{"name", name}, {"requiredBy", parent}, {"version", version},
                {"request", QVariantMap{{"name", name}, {"version", version}, {"rootHash", version}}}};
    }
    static QVariantList response(QVariantMap graph, QVariantList offers = {},
                                 QVariantList extra = {}) {
        extra.append(QVariantMap{{"name", "chat"}, {"version", "1.0.0"},
            {"dependencyGraph", graph}, {"optionalDependencies", offers}});
        return extra;
    }
    static QStringList names(QString request) {
        QStringList out;
        for (const auto& v : QJsonDocument::fromJson(request.toUtf8()).array())
            out.append(v.toObject().value("name").toString());
        return out;
    }
private slots:
    void invalidatedOptionalVersionUsesTheNewCheckedDefault() {
        logos::OptionalDependencyPreview preview("chat", "repo", {}, {}, {{"rln", "0.9.0"}});
        const auto next = preview.advance(response({}, {offer("rln", "chat", "1.0.0")}));
        const auto inputs = QJsonDocument::fromJson(next.toUtf8()).array();
        QCOMPARE(inputs.size(), 2);
        QCOMPARE(inputs[1].toObject().value("version").toString(), QStringLiteral("1.0.0"));
        QCOMPARE(preview.optionalNames(), QStringList{"rln"});
    }
    void selectedRlnExposesMandatoryClosureAndNestedOptionals() {
        logos::OptionalDependencyPreview preview("chat", "repo");
        const auto rln = offer("rln", "delivery");
        QCOMPARE(names(preview.advance(response({{"chat", QStringList{"delivery"}}}, {rln}))),
                 (QStringList{"chat", "rln"}));
        const auto next = preview.advance(response({{"chat", QStringList{"delivery"}},
            {"rln", QStringList{"lez_rln"}}}, {offer("rpc", "lez_rln")},
            {QVariantMap{{"name", "rln"}, {"version", "1.0.0"}}, QVariantMap{{"name", "lez_rln"}}}));
        QCOMPARE(names(next), (QStringList{"chat", "rln", "rpc"}));
        QCOMPARE(preview.optionalNames(), (QStringList{"rln", "rpc"}));
        QCOMPARE(preview.offers().size(), 2);
    }
    void uncheckedParentPrunesItsClosureAndStaleMandatoryPins() {
        logos::OptionalDependencyPreview preview("chat", "repo", {{"lez_rln", "0.9.0"}},
                                                {{"rln", false}}, {{"rln", "1.0.0"}});
        const auto next = preview.advance(response({{"chat", QStringList{"delivery"}}},
                                                   {offer("rln", "delivery")}));
        QCOMPARE(names(next), QStringList{"chat"});
        QVERIFY(preview.optionalNames().isEmpty());
        QCOMPARE(preview.offers().size(), 1); // still available to recheck
    }
    void requiredChildPinsApplyOnlyWhenItsParentIsSelected() {
        logos::OptionalDependencyPreview preview("chat", "repo", {{"lez_rln", "0.9.0"}});
        preview.advance(response({{"chat", QStringList{"delivery"}}}, {offer("rln", "delivery")}));
        const auto next = preview.advance(response({{"chat", QStringList{"delivery"}},
            {"rln", QStringList{"lez_rln"}}}));
        QCOMPARE(names(next), (QStringList{"chat", "lez_rln", "rln"}));
    }
    void mandatorySharedPackageLosesItsOptionalCheckbox() {
        logos::OptionalDependencyPreview preview("chat", "repo", {}, {{"rpc", false}});
        preview.advance(response({{"chat", QStringList{"delivery"}}},
            {offer("rln", "delivery"), offer("rpc", "delivery")}));
        preview.advance(response({{"chat", QStringList{"delivery"}}, {"rln", QStringList{"rpc"}}}));
        QCOMPARE(preview.offers().size(), 1);
        QCOMPARE(preview.optionalNames(), QStringList{"rln"});
    }
    void changedVersionDropsOffersOfThePreviousArtifact() {
        logos::OptionalDependencyPreview preview("chat", "repo");
        preview.advance(response({{"chat", QStringList{"delivery"}}}, {offer("rln", "delivery")},
            {QVariantMap{{"name", "delivery"}, {"version", "1.0.0"}}}));
        const auto next = preview.advance(response({{"chat", QStringList{"delivery"}}}, {},
            {QVariantMap{{"name", "delivery"}, {"version", "2.0.0"}}}));
        QCOMPARE(names(next), QStringList{"chat"});
        QVERIFY(preview.offers().isEmpty());
    }
    void nestedOptionalCyclesConvergeWithoutDuplicateRequests() {
        logos::OptionalDependencyPreview preview("chat", "repo");
        preview.advance(response({}, {offer("rln", "chat")}));
        const auto second = preview.advance(response({}, {offer("rpc", "rln"), offer("rln", "rpc")}));
        QCOMPARE(names(second), (QStringList{"chat", "rln", "rpc"}));
        QCOMPARE(preview.advance(response({})), second);
    }
    void unavailableOptionalRemainsUncheckedWithoutBlockingOtherOffers() {
        logos::OptionalDependencyPreview preview("chat", "repo");
        auto missing = offer("missing", "chat"); missing.insert("error", "not available");
        QCOMPARE(names(preview.advance(response({}, {missing, offer("rln", "chat")}))),
                 (QStringList{"chat", "rln"}));
        QCOMPARE(preview.offers().size(), 2);
        QCOMPARE(preview.optionalNames(), QStringList{"rln"});
    }
    void satisfiedInstalledParentStillMakesOffersReachable() {
        logos::OptionalDependencyPreview preview("chat", "repo");
        QCOMPARE(names(preview.advance(response({{"chat", QStringList{"installed"}},
            {"installed", QStringList{"delivery"}}}, {offer("rln", "delivery")}))),
            (QStringList{"chat", "rln"}));
    }
};
QTEST_GUILESS_MAIN(OptionalDependencyPreviewTest)
#include "optional_dependency_preview_test.moc"
