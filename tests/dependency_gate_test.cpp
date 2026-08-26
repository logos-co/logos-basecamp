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
// The rows below are VERBATIM wire payloads, captured by driving a real
// logoscore daemon over a real installed tree with a module declaring
// `{"name":"depsvc","version":"…","signer":"did:jwk:…"}` against `depsvc`
// 1.0.0. They are parsed with QJsonDocument rather than hand-built as
// QVariantMaps so this suite consumes exactly the bytes the module emits — a
// hand-built fixture can drift from the wire and still pass.
//
// The signer rows come from the same rig, varying only the `signer` sidecar in
// depsvc's install directory: absent -> signer_unknown, a different DID ->
// signer_mismatch. The declared range is ^1.0.0 on those two so the SIGNER
// verdict is the one reported; with ^2.0.0 and a mismatched signer the same
// rig reports signer_mismatch (identity outranks a range) and with ^2.0.0 and
// a matching signer it reports version_mismatch.
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

    // depsvc 1.0.0 installed and its recorded publisher is NOT the pinned
    // one: a package under the right name from the wrong signer.
    static QVariant signerMismatchRow()
    {
        return wireRow(
            R"({"installType":"user","name":"depsvc",)"
            R"("observedSigner":"did:jwk:SOMEBODY_ELSE",)"
            R"("requiredSigner":"did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0",)"
            R"("requiredVersion":"^1.0.0","status":"signer_mismatch",)"
            R"("version":"1.0.0"})");
    }

    // depsvc 1.0.0 installed with NO recorded publisher — the state of every
    // embedded package and everything installed before the record existed.
    // The pin cannot be checked; there is no observedSigner key at all.
    static QVariant signerUnknownRow()
    {
        return wireRow(
            R"({"installType":"user","name":"depsvc",)"
            R"("requiredSigner":"did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0",)"
            R"("requiredVersion":"^1.0.0","status":"signer_unknown",)"
            R"("version":"1.0.0"})");
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

    // A package under the right name from the WRONG PUBLISHER. It is not the
    // dependency the module named, and no version of it ever will be, so the
    // load must not proceed on top of it.
    void a_signer_mismatch_blocks()
    {
        const auto b = readDependencyBlocker(signerMismatchRow());
        QCOMPARE(b.kind, DependencyBlockKind::SignerMismatch);
        QCOMPARE(b.name, QStringLiteral("depsvc"));
        QCOMPARE(b.requiredSigner,
                 QStringLiteral("did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0"));
        QCOMPARE(b.observedSigner, QStringLiteral("did:jwk:SOMEBODY_ELSE"));
    }

    // THE DESIGN CALL, at the gate. A pin that could not be CHECKED — nothing
    // records who published the installed package — does not block.
    //
    // Absence of evidence is not evidence of mismatch, and this is the normal
    // state for two whole populations: every embedded package (placed by the
    // build, never through the installer that records a publisher, so it can
    // NEVER acquire one) and everything installed before the record existed.
    // Blocking here would make a pin on an embedded dependency unsatisfiable
    // by construction, forever, with no action a user could take.
    //
    // The package manager owns this call and can flip it in ONE place
    // (UnknownSignerPolicy::Strict makes its scanner emit signer_mismatch
    // instead, which this gate already blocks) — which is exactly why this
    // gate must not second-guess it. A decision, pinned, not an omission.
    void a_signer_that_cannot_be_checked_does_not_block()
    {
        const auto b = readDependencyBlocker(signerUnknownRow());
        QCOMPARE(b.kind, DependencyBlockKind::None);
        // The facts still arrive, so a caller that wants to SHOW the gap can.
        QCOMPARE(b.requiredSigner,
                 QStringLiteral("did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0"));
        QVERIFY(b.observedSigner.isEmpty());
    }

    // ── Blocking a load and being on disk are DIFFERENT questions ───────
    // The same rows feed two consumers: the load gate, and the forward edges
    // of the dependency graph the uninstall plan walks. A version_mismatch
    // row answers YES to both — it blocks the load AND the package is sitting
    // on disk. Collapsing the two into one predicate drops the mismatched
    // dependency out of the graph while resolveFlatDependents still reports
    // the reverse edge, and an uninstall plan walking an asymmetric graph is
    // wrong in a way nobody traces back to a load gate.
    void a_mismatched_dependency_blocks_a_load_and_is_still_on_disk()
    {
        const auto b = readDependencyBlocker(mismatchRow());
        QCOMPARE(b.kind, DependencyBlockKind::VersionMismatch);  // blocks
        QVERIFY(logos::dependencyIsPresent(b));                  // and is present
    }

    void a_signer_mismatched_dependency_blocks_a_load_and_is_still_on_disk()
    {
        const auto b = readDependencyBlocker(signerMismatchRow());
        QCOMPARE(b.kind, DependencyBlockKind::SignerMismatch);  // blocks
        QVERIFY(logos::dependencyIsPresent(b));                 // and is present
    }

    void only_an_absent_row_is_treated_as_not_present()
    {
        QVERIFY(!logos::dependencyIsPresent(readDependencyBlocker(absentRow())));
        QVERIFY(logos::dependencyIsPresent(readDependencyBlocker(satisfiedRow())));
        // A cycle row keeps the graph edge, which is what the gate did before
        // any of this — the cycle is a property of the edge, not evidence the
        // package is missing.
        QVERIFY(logos::dependencyIsPresent(readDependencyBlocker(
            wireRow(R"({"name":"a","status":"cycle","version":"","installType":""})"))));
    }

    // ── The whole split, as the coordinator consumes it ─────────────────
    // PackageCoordinator's async lambda is not reachable from a unit test, so
    // the split it performs lives in the header and is exercised here. The
    // regression this guards: a mismatched dependency landing in `blocking`
    // but NOT in `present`, which silently deletes a forward edge from the
    // graph the uninstall plan walks while resolveFlatDependents keeps
    // reporting the reverse one.
    void the_split_puts_a_mismatched_dependency_in_both_lists()
    {
        const auto split = logos::splitDependencyRows({mismatchRow()});
        QCOMPARE(split.present,  QStringList{QStringLiteral("depsvc")});
        QCOMPARE(split.blocking, QStringList{QStringLiteral("depsvc")});
        QCOMPARE(split.blockers.size(), 1);
        QCOMPARE(split.blockers.first().toMap().value("detail").toString(),
                 QStringLiteral("requires ^2.0.0, found 1.0.0"));
    }

    void the_split_keeps_an_absent_dependency_out_of_the_graph()
    {
        const auto split = logos::splitDependencyRows({absentRow()});
        QVERIFY(split.present.isEmpty());
        QCOMPARE(split.blocking, QStringList{QStringLiteral("depsvc")});
    }

    void the_split_leaves_a_satisfied_dependency_unblocked()
    {
        const auto split = logos::splitDependencyRows({satisfiedRow()});
        QCOMPARE(split.present, QStringList{QStringLiteral("depsvc")});
        QVERIFY(split.blocking.isEmpty());
        QVERIFY(split.blockers.isEmpty());
    }

    // A row with no name is not a dependency; there is nothing to act on, and
    // dropping it is what the gate has always done.
    void the_split_drops_a_row_that_names_nothing()
    {
        const auto split = logos::splitDependencyRows({
            wireRow(R"({"name":"","status":"not_installed","version":""})")});
        QVERIFY(split.present.isEmpty());
        QVERIFY(split.blocking.isEmpty());
        QVERIFY(split.blockers.isEmpty());
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

    // A THIRD sentence, because it is a third problem. "not installed" sends
    // the user to install a package they have; "requires ^2.0.0, found 1.0.0"
    // sends them after a version that will never satisfy this, because the
    // package on disk is somebody else's. Only naming the publisher does.
    void a_signer_mismatch_says_publisher_not_version_and_names_both_dids()
    {
        const QString detail =
            dependencyBlockerDetail(readDependencyBlocker(signerMismatchRow()));
        QVERIFY2(detail.startsWith(QStringLiteral("published by a different signer")),
                 qPrintable(detail));
        // Must not read as either of the other two remedies.
        QVERIFY2(!detail.contains(QStringLiteral("not installed")), qPrintable(detail));
        QVERIFY2(!detail.startsWith(QStringLiteral("requires")), qPrintable(detail));
        // Both DIDs: the pin alone does not say what went wrong, and the
        // observation alone is an accusation with no charge attached.
        QVERIFY2(detail.contains(QStringLiteral("did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0")),
                 qPrintable(detail));
        QVERIFY2(detail.contains(QStringLiteral("did:jwk:SOMEBODY_ELSE")), qPrintable(detail));
    }

    // Defensive: a signer-mismatch row that lost one DID still reads as a
    // publisher problem rather than collapsing to an empty clause.
    void a_signer_mismatch_with_a_missing_did_still_reads()
    {
        logos::DependencyBlocker b;
        b.kind = DependencyBlockKind::SignerMismatch;
        b.name = QStringLiteral("depsvc");
        QCOMPARE(dependencyBlockerDetail(b),
                 QStringLiteral("published by a different signer"));
        b.requiredSigner = QStringLiteral("did:jwk:PINNED");
        QCOMPARE(dependencyBlockerDetail(b),
                 QStringLiteral("published by a different signer; requires did:jwk:PINNED"));
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

    // The kind QML switches on. This used to be a ternary
    // (`VersionMismatch ? "version_mismatch" : "not_installed"`), so a signer
    // mismatch would have crossed into QML labelled "not_installed" and the
    // dialog would have told the user to install a package sitting on disk —
    // the same trailing-else failure this whole header exists to prevent, one
    // layer out.
    void the_wire_map_carries_the_signer_kind_and_both_dids()
    {
        const QVariantMap m = dependencyBlockerToMap(readDependencyBlocker(signerMismatchRow()));
        QCOMPARE(m.value("kind").toString(), QStringLiteral("signer_mismatch"));
        QCOMPARE(m.value("requiredSigner").toString(),
                 QStringLiteral("did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0"));
        QCOMPARE(m.value("observedSigner").toString(), QStringLiteral("did:jwk:SOMEBODY_ELSE"));
        // Still on disk, so the row keeps the version it has.
        QCOMPARE(m.value("installedVersion").toString(), QStringLiteral("1.0.0"));
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

    void summary_of_only_signer_blockers_is_signer()
    {
        QVariantList l;
        l << dependencyBlockerToMap(readDependencyBlocker(signerMismatchRow()));
        QCOMPARE(summariseDependencyBlockers(l), QStringLiteral("signer"));
    }

    // The summariser used to test ONE kind and sweep the rest into `absent`,
    // so a pure signer set would have been summarised as "not installed".
    void summary_of_a_signer_and_a_version_blocker_is_mixed()
    {
        QVariantList l;
        l << dependencyBlockerToMap(readDependencyBlocker(signerMismatchRow()));
        l << dependencyBlockerToMap(readDependencyBlocker(mismatchRow()));
        QCOMPARE(summariseDependencyBlockers(l), QStringLiteral("mixed"));
    }

    void summary_of_a_signer_and_an_absent_blocker_is_mixed()
    {
        QVariantList l;
        l << dependencyBlockerToMap(readDependencyBlocker(signerMismatchRow()));
        l << dependencyBlockerToMap(readDependencyBlocker(absentRow()));
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
