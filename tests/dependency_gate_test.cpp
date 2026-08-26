#include <QtTest/QtTest>

#include <QJsonDocument>
#include <QVariantList>
#include <QVariantMap>

#include "utils/DependencyBlocker.h"

using logos::DependencyBlockKind;
using logos::dependencyBlockerDetail;
using logos::dependencyBlockerToMap;
using logos::readDependencyBlocker;
using logos::summariseDependencyBlockers;

// The load gate in front of a UI plugin: given what
// `package_manager.resolveFlatDependencies` reported about a plugin's
// transitive dependencies, may the plugin be loaded, and if not, what does the
// user need to be told?
//
// The three rows below are VERBATIM wire payloads, captured by driving a real
// logoscore daemon over a real installed tree (`~/deps-drive/gate.sh`) with the
// ui_qml plugin `depui` declaring `{"name":"depsvc","version":"^2.0.0",
// "signer":"did:jwk:…"}` against `depsvc` 1.0.0. They are parsed with
// QJsonDocument rather than hand-built as QVariantMaps so this suite consumes
// exactly the bytes the module emits — a hand-built fixture can drift from the
// wire and still pass.
//
// What makes the mismatch row a gate problem rather than a display one: the
// gate classified BY EXCLUSION —
//
//     if (m.value("status").toString() == "not_installed") missing << s;
//     else                                                 installed << s;
//
// so a status invented after that line was written lands in `installed` and
// the plugin is admitted on top of a dependency the resolver rejected. There
// is no diagnostic; the user sees a bare "plugin load failed" from liblogos,
// or worse, a plugin that mounts and misbehaves.
class DependencyGateTest : public QObject {
    Q_OBJECT

    static QVariant wireRow(const char* json)
    {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json), &err);
        Q_ASSERT(err.error == QJsonParseError::NoError);
        return doc.object().toVariantMap();
    }

    // depsvc 1.0.0 installed, edge declared bare — nothing to complain about.
    static QVariant satisfiedRow()
    {
        return wireRow(R"({"installType":"user","name":"depsvc",)"
                       R"("status":"installed","version":"1.0.0"})");
    }

    // depsvc 1.0.0 installed, edge declared ^2.0.0 — installed, unusable.
    static QVariant mismatchRow()
    {
        return wireRow(
            R"({"installType":"user","name":"depsvc",)"
            R"("requiredSigner":"did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0",)"
            R"("requiredVersion":"^2.0.0","status":"version_mismatch",)"
            R"("version":"1.0.0"})");
    }

    // depsvc removed from the tree entirely — absent outranks the range, and
    // the declared range still rides along so the message can name it.
    static QVariant absentRow()
    {
        return wireRow(
            R"({"installType":"","name":"depsvc",)"
            R"("requiredSigner":"did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0",)"
            R"("requiredVersion":"^2.0.0","status":"not_installed",)"
            R"("version":""})");
    }

private slots:
    // ── What blocks a load ──────────────────────────────────────────────
    void a_satisfied_dependency_does_not_block()
    {
        const auto b = readDependencyBlocker(satisfiedRow());
        QCOMPARE(b.kind, DependencyBlockKind::None);
        QCOMPARE(b.name, QStringLiteral("depsvc"));
    }

    void an_absent_dependency_blocks()
    {
        const auto b = readDependencyBlocker(absentRow());
        QCOMPARE(b.kind, DependencyBlockKind::NotInstalled);
        QCOMPARE(b.name, QStringLiteral("depsvc"));
    }

    // THE ONE THIS FILE EXISTS FOR. An installed dependency of the wrong
    // version is not a satisfied dependency.
    void a_version_mismatch_blocks()
    {
        const auto b = readDependencyBlocker(mismatchRow());
        QCOMPARE(b.kind, DependencyBlockKind::VersionMismatch);
        QCOMPARE(b.name, QStringLiteral("depsvc"));
    }

    // ── What the user is told ───────────────────────────────────────────
    // A mismatch message that names neither the constraint nor what is
    // actually installed leaves the user with nothing to act on: they can see
    // the plugin refused to load and cannot tell which version to go and get.
    void a_mismatch_names_the_constraint_and_the_installed_version()
    {
        const QString detail = dependencyBlockerDetail(readDependencyBlocker(mismatchRow()));
        QCOMPARE(detail, QStringLiteral("requires ^2.0.0, found 1.0.0"));
    }

    void an_absent_dependency_still_says_absent()
    {
        const QString detail = dependencyBlockerDetail(readDependencyBlocker(absentRow()));
        // Must not read as a version complaint — the remedy is "install it",
        // and there is no installed version to report.
        QVERIFY(detail.startsWith(QStringLiteral("not installed")));
        QCOMPARE(detail, QStringLiteral("not installed; requires ^2.0.0"));
    }

    void an_absent_dependency_with_no_declared_range_says_only_that()
    {
        logos::DependencyBlocker b;
        b.kind = DependencyBlockKind::NotInstalled;
        b.name = QStringLiteral("depsvc");
        QCOMPARE(dependencyBlockerDetail(b), QStringLiteral("not installed"));
    }

    // Defensive: a blocking row that carries only half the facts must still
    // render a usable clause rather than a dangling dash.
    void a_mismatch_with_no_declared_range_still_reads()
    {
        logos::DependencyBlocker b;
        b.kind = DependencyBlockKind::VersionMismatch;
        b.name = QStringLiteral("depsvc");
        b.installedVersion = QStringLiteral("1.0.0");
        QCOMPARE(dependencyBlockerDetail(b),
                 QStringLiteral("installed version 1.0.0 was rejected"));
    }

    void a_satisfied_row_has_no_detail()
    {
        QCOMPARE(dependencyBlockerDetail(readDependencyBlocker(satisfiedRow())), QString());
    }

    // ── The payload QML renders ─────────────────────────────────────────
    void the_wire_map_carries_the_kind_and_both_versions()
    {
        const QVariantMap m = dependencyBlockerToMap(readDependencyBlocker(mismatchRow()));
        QCOMPARE(m.value("name").toString(),             QStringLiteral("depsvc"));
        QCOMPARE(m.value("kind").toString(),             QStringLiteral("version_mismatch"));
        QCOMPARE(m.value("requiredVersion").toString(),  QStringLiteral("^2.0.0"));
        QCOMPARE(m.value("installedVersion").toString(), QStringLiteral("1.0.0"));
        QCOMPARE(m.value("detail").toString(),
                 QStringLiteral("requires ^2.0.0, found 1.0.0"));
    }

    void the_wire_map_for_an_absent_dependency_reports_no_installed_version()
    {
        const QVariantMap m = dependencyBlockerToMap(readDependencyBlocker(absentRow()));
        QCOMPARE(m.value("kind").toString(), QStringLiteral("not_installed"));
        QVERIFY(m.value("installedVersion").toString().isEmpty());
    }

    // ── Choosing the headline ───────────────────────────────────────────
    // The dialog says one of three different things; a set containing both
    // kinds must not claim to be either one of them.
    void summary_of_only_absent_blockers_is_absent()
    {
        QVariantList l;
        l << dependencyBlockerToMap(readDependencyBlocker(absentRow()));
        QCOMPARE(summariseDependencyBlockers(l), QStringLiteral("absent"));
    }

    void summary_of_only_mismatched_blockers_is_mismatch()
    {
        QVariantList l;
        l << dependencyBlockerToMap(readDependencyBlocker(mismatchRow()));
        QCOMPARE(summariseDependencyBlockers(l), QStringLiteral("mismatch"));
    }

    void summary_of_both_kinds_is_mixed()
    {
        QVariantList l;
        l << dependencyBlockerToMap(readDependencyBlocker(absentRow()));
        l << dependencyBlockerToMap(readDependencyBlocker(mismatchRow()));
        QCOMPARE(summariseDependencyBlockers(l), QStringLiteral("mixed"));
    }

    void summary_of_nothing_blocking_is_empty()
    {
        QCOMPARE(summariseDependencyBlockers({}), QString());
    }

    // ── Statuses this build deliberately admits ─────────────────────────
    // Both of these are decisions, not oversights. `cycle` is admitted
    // because nothing here has been driven against a real cyclic install and
    // blocking it would be a behaviour change made blind. An unrecognised
    // status is admitted because this gate is an ADVISORY pre-check in front
    // of liblogos' own resolver — liblogos decides whether a load succeeds,
    // and refusing on a word this build does not know would block loads that
    // work. Change either only with a run behind it.
    void a_cycle_is_admitted_today()
    {
        const auto b = readDependencyBlocker(
            wireRow(R"({"name":"a","status":"cycle","version":"","installType":""})"));
        QCOMPARE(b.kind, DependencyBlockKind::None);
    }

    void an_unrecognised_status_is_admitted_rather_than_guessed_at()
    {
        const auto b = readDependencyBlocker(
            wireRow(R"({"name":"a","status":"quarantined","version":"1.0.0"})"));
        QCOMPARE(b.kind, DependencyBlockKind::None);
    }
};

QTEST_MAIN(DependencyGateTest)
#include "dependency_gate_test.moc"
