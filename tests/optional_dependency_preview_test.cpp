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
    void initialRequestCarriesRequiredRowPins() {
        logos::OptionalDependencyPreview preview("chat", "repo", {{"delivery", "0.2.0"}});
        QCOMPARE(names(preview.initialRequest()), (QStringList{"chat", "delivery"}));
    }
    void reofferedSelectionSurvivesAParentVersionChange() {
        logos::OptionalDependencyPreview preview("chat", "repo", {{"delivery", "0.2.0"}});
        const QVariantMap graph{{"chat", QStringList{"delivery"}}};
        auto delivery = [](QString v) { return QVariantMap{{"name", "delivery"}, {"version", v}}; };
        const auto first = preview.advance(response(graph, {offer("rln", "delivery")}, {delivery("0.3.0")}));
        QVERIFY(names(first).contains("rln"));
        // The resolver keeps listing a selected optional; the parent's new version must not drop it.
        const auto second = preview.advance(response(graph, {offer("rln", "delivery")}, {delivery("0.2.0")}));
        QCOMPARE(names(second), names(first));
        QCOMPARE(preview.advance(response(graph, {offer("rln", "delivery")}, {delivery("0.2.0")})), second);
    }
    void duplicateRowsOfOneNameDoNotDropItsOffers() {
        logos::OptionalDependencyPreview preview("chat", "repo");
        const QVariantMap graph{{"chat", QStringList{"d"}}};
        const QVariantList rows{QVariantMap{{"name", "d"}, {"version", "1.0.0"}},
                                QVariantMap{{"name", "d"}, {"version", "2.0.0"}}};
        const auto first = preview.advance(response(graph, {offer("q", "d")}, rows));
        QCOMPARE(preview.advance(response(graph, {offer("q", "d")}, rows)), first);
        QCOMPARE(preview.optionalNames(), QStringList{"q"});
    }
    void availableOfferWinsOverAnUnavailableOneOfTheSameName() {
        logos::OptionalDependencyPreview preview("chat", "repo");
        auto broken = offer("o", "a_mod");
        broken.insert("error", "no candidate");
        preview.advance(response({{"chat", QStringList{"a_mod", "b_mod"}}}, {broken, offer("o", "b_mod")}));
        QCOMPARE(preview.offers().size(), 1);
        QVERIFY(!preview.offers().first().toMap().contains("error"));
        QCOMPARE(preview.optionalNames(), QStringList{"o"});
    }
    static QVariantMap versioned(QString name, QString parent) {
        auto o = offer(name, parent, "2.0.0");
        o.insert("versions", QVariantList{
            QVariantMap{{"manifest", QVariantMap{{"version", "2.0.0"}}}, {"rootHash", "2.0.0"}},
            QVariantMap{{"manifest", QVariantMap{{"version", "1.0.0"}}}, {"rootHash", "1.0.0"}}});
        return o;
    }
    void installedOptionalIsResolvedAtItsInstalledRelease() {
        logos::OptionalDependencyPreview preview("chat", "repo");
        preview.setInstalled({{"rln", {"1.0.0", "1.0.0"}}});
        const auto next = preview.advance(response({{"chat", QStringList{"delivery"}}}, {versioned("rln", "delivery")}));
        // Resolved so its required packages (e.g. lez_rln) are listed.
        const auto inputs = QJsonDocument::fromJson(next.toUtf8()).array();
        QCOMPARE(inputs.size(), 2);
        QCOMPARE(inputs[1].toObject().value("version").toString(), QStringLiteral("1.0.0"));
        QCOMPARE(preview.offers().size(), 1);
        const auto shown = preview.offers().first().toMap();
        QCOMPARE(shown.value("version").toString(), QStringLiteral("1.0.0"));
        QCOMPARE(shown.value("installedVersion").toString(), QStringLiteral("1.0.0"));
        QVERIFY(shown.value("selected").toBool());
    }
    void pickingAnotherReleaseOfAnInstalledOptionalRequestsIt() {
        logos::OptionalDependencyPreview preview("chat", "repo", {}, {}, {{"rln", "2.0.0"}});
        preview.setInstalled({{"rln", {"1.0.0", "1.0.0"}}});
        const auto next = preview.advance(response({{"chat", QStringList{"delivery"}}}, {versioned("rln", "delivery")}));
        const auto inputs = QJsonDocument::fromJson(next.toUtf8()).array();
        QCOMPARE(inputs.size(), 2);
        QCOMPARE(inputs[1].toObject().value("version").toString(), QStringLiteral("2.0.0"));
    }
    void newOptionalsCanStartUnselected() {
        logos::OptionalDependencyPreview preview("chat", "repo");
        preview.setSelectNew(false);
        const auto next = preview.advance(response({{"chat", QStringList{"delivery"}}}, {offer("rln", "delivery")}));
        QCOMPARE(names(next), QStringList{"chat"});
        QVERIFY(!preview.offers().first().toMap().value("selected").toBool());
    }
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
