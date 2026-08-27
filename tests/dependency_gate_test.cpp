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
// The rows below are VERBATIM wire payloads captured from a real logoscore
// daemon over a real installed tree, parsed with QJsonDocument rather than
// hand-built as QVariantMaps so this suite consumes exactly the bytes the
// module emits — a hand-built fixture can drift from the wire and still pass.
//
// depsvc 1.0.0 is installed from a really-signed .lgx; only the `signer` PIN
// in the depending manifest varies across the signer rows:
//
//   pin = the DID that signed depsvc      -> installed        (+ signerDid)
//   pin = a DIFFERENT, REAL did:jwk       -> signer_mismatch  (+ signerDid)
//   depsvc installed with no manifest.sig -> signer_unknown   (no signerDid)
//
// THE PIN MUST CARRY A KEY: the verdict comes from extracting the Ed25519
// public key out of the pinned did:jwk and verifying depsvc's signature with
// it. The version and absence rows still pin
// `did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0` = {"kty":"OKP",
// "crv":"Ed25519"} — no `x` member, so no key. Correct for those two only
// because neither carries a signature, so the scanner never parses the pin.
// Reuse it anywhere a signature IS checked and it fails closed as
// signer_mismatch: green for the wrong reason.
class DependencyGateTest : public QObject {
    Q_OBJECT

    static QVariant wireRow(const char* json)
    {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json), &err);
        Q_ASSERT(err.error == QJsonParseError::NoError);
        return doc.object().toVariantMap();
    }

    // Edge declared bare — nothing to complain about.
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

    // Absent outranks the range, and the range still rides along so the message
    // can name it.
    static QVariant absentRow()
    {
        return wireRow(
            R"({"installType":"","name":"depsvc",)"
            R"("requiredSigner":"did:jwk:eyJrdHkiOiJPS1AiLCJjcnYiOiJFZDI1NTE5In0",)"
            R"("requiredVersion":"^2.0.0","status":"not_installed",)"
            R"("version":""})");
    }

    // Really signed by one key, and the depending manifest pins a DIFFERENT
    // real key. The two DIDs differing is the NORMAL shape of this row, and is
    // NOT what produced the verdict — the scanner verified under the pin's key
    // and failed. See readDependencyBlocker.
    static QVariant signerMismatchRow()
    {
        return wireRow(
            R"({"installType":"user","name":"depsvc",)"
            R"("requiredSigner":"did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6Il96N1dqMUd4RWhEdURHQ1hHdVJuOUdTdm1teHo3ZGtMY0dvaEdqMTJOMEUifQ",)"
            R"("requiredVersion":"^1.0.0",)"
            R"("signerDid":"did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ",)"
            R"("status":"signer_mismatch","version":"1.0.0"})");
    }

    // No manifest.sig, so no signerDid at all.
    static QVariant signerUnknownRow()
    {
        return wireRow(
            R"({"installType":"user","name":"depsvc",)"
            R"("requiredSigner":"did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ",)"
            R"("requiredVersion":"^1.0.0",)"
            R"("status":"signer_unknown","version":"1.0.0"})");
    }

    // The pin NAMES the signing key, so it is satisfied and the row is ordinary
    // `installed` — while still carrying `signerDid`, which is a property of
    // the PACKAGE, not of the edge. A gate reading "a signerDid is present" as
    // a signal would block a perfectly satisfied dependency.
    static QVariant signerSatisfiedRow()
    {
        return wireRow(
            R"({"installType":"user","name":"depsvc",)"
            R"("requiredSigner":"did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ",)"
            R"("requiredVersion":"^1.0.0",)"
            R"("signerDid":"did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ",)"
            R"("status":"installed","version":"1.0.0"})");
    }

private slots:
    // What blocks a load.
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

    void a_version_mismatch_blocks()
    {
        const auto b = readDependencyBlocker(mismatchRow());
        QCOMPARE(b.kind, DependencyBlockKind::VersionMismatch);
        QCOMPARE(b.name, QStringLiteral("depsvc"));
    }

    void a_signer_mismatch_blocks()
    {
        const auto b = readDependencyBlocker(signerMismatchRow());
        QCOMPARE(b.kind, DependencyBlockKind::SignerMismatch);
        QCOMPARE(b.name, QStringLiteral("depsvc"));
        QCOMPARE(b.requiredSigner, QStringLiteral("did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6Il96N1dqMUd4RWhEdURHQ1hHdVJuOUdTdm1teHo3ZGtMY0dvaEdqMTJOMEUifQ"));
        QCOMPARE(b.signerDid,      QStringLiteral("did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ"));
    }

    // THE DESIGN CALL, pinned here. A pin with no signature to check it against
    // does not block, because that is the normal state for every embedded
    // package — placed by the build, never through installPluginFile, so it can
    // NEVER carry a manifest.sig — and blocking would make those unpinnable by
    // construction. See DependencyBlocker.h; the package manager can flip it.
    void a_signer_that_cannot_be_checked_does_not_block()
    {
        const auto b = readDependencyBlocker(signerUnknownRow());
        QCOMPARE(b.kind, DependencyBlockKind::None);
        // The facts still arrive, so a caller that wants to SHOW the gap can.
        QCOMPARE(b.requiredSigner, QStringLiteral("did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ"));
        QVERIFY(b.signerDid.isEmpty());
    }

    // The other half of that call: a satisfied pin still carries a signerDid, so
    // its presence is not a problem signal. A gate that blocked on it would
    // refuse every correctly-signed dependency.
    void a_satisfied_signer_pin_does_not_block()
    {
        const auto b = readDependencyBlocker(signerSatisfiedRow());
        QCOMPARE(b.kind, DependencyBlockKind::None);
        QVERIFY(logos::dependencyIsPresent(b));
        // Both DIDs arrive EQUAL — what a satisfied pin looks like, and why
        // equality must not be what a verdict is derived from: this row is
        // indistinguishable from one that was relabelled.
        QCOMPARE(b.requiredSigner, QStringLiteral("did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ"));
        QCOMPARE(b.signerDid,      QStringLiteral("did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ"));
        // And no detail clause: there is nothing for a user to do.
        QVERIFY(dependencyBlockerDetail(b).isEmpty());
    }

    // Blocking a load and being on disk are DIFFERENT questions; a
    // version_mismatch row answers yes to both. One predicate for both drops
    // the mismatched dependency out of the graph while resolveFlatDependents
    // still reports the reverse edge, and an uninstall plan walking an
    // asymmetric graph is wrong in a way nobody traces back to a load gate.
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
        // A cycle is a property of the edge, not evidence the package is
        // missing, so the row keeps its graph edge.
        QVERIFY(logos::dependencyIsPresent(readDependencyBlocker(
            wireRow(R"({"name":"a","status":"cycle","version":"","installType":""})"))));
    }

    // The whole split, as the coordinator consumes it. Its async lambda is not
    // reachable from a unit test, so the split lives in the header and is
    // exercised here.
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

    // A row with no name is not a dependency: nothing to act on.
    void the_split_drops_a_row_that_names_nothing()
    {
        const auto split = logos::splitDependencyRows({
            wireRow(R"({"name":"","status":"not_installed","version":""})")});
        QVERIFY(split.present.isEmpty());
        QVERIFY(split.blocking.isEmpty());
        QVERIFY(split.blockers.isEmpty());
    }

    // What the user is told. A mismatch naming neither the constraint nor what
    // is installed leaves them unable to tell which version to go and get.
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

    // Empty, so the row renders as just the name. The heading already says
    // these are not installed, and every dependency in the fleet is a bare
    // name — a per-row repeat would change that text for all of them, which
    // is what broke basecamp-missing-deps.
    void an_absent_dependency_with_no_declared_range_adds_nothing()
    {
        logos::DependencyBlocker b;
        b.kind = DependencyBlockKind::NotInstalled;
        b.name = QStringLiteral("depsvc");
        QCOMPARE(dependencyBlockerDetail(b), QString());
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

    // A third sentence for a third problem: "not installed" sends the user
    // after a package they have, and a version clause after a version that can
    // never satisfy this. Only naming the keys works.
    void a_signer_mismatch_says_signer_not_version_and_names_both_dids()
    {
        const QString detail =
            dependencyBlockerDetail(readDependencyBlocker(signerMismatchRow()));
        QVERIFY2(detail.startsWith(QStringLiteral("signed by a different key")),
                 qPrintable(detail));
        // Must not read as either of the other two remedies.
        QVERIFY2(!detail.contains(QStringLiteral("not installed")), qPrintable(detail));
        QVERIFY2(!detail.startsWith(QStringLiteral("requires")), qPrintable(detail));
        // Both DIDs: the pin alone does not say what went wrong, and the
        // installed one alone is an accusation with no charge attached.
        QVERIFY2(detail.contains(QStringLiteral("did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6Il96N1dqMUd4RWhEdURHQ1hHdVJuOUdTdm1teHo3ZGtMY0dvaEdqMTJOMEUifQ")), qPrintable(detail));
        QVERIFY2(detail.contains(QStringLiteral("did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ")), qPrintable(detail));
    }

    // Defensive: a signer-mismatch row that lost one DID still reads as a
    // signer problem rather than collapsing to an empty clause.
    void a_signer_mismatch_with_a_missing_did_still_reads()
    {
        logos::DependencyBlocker b;
        b.kind = DependencyBlockKind::SignerMismatch;
        b.name = QStringLiteral("depsvc");
        QCOMPARE(dependencyBlockerDetail(b),
                 QStringLiteral("signed by a different key"));
        b.requiredSigner = QStringLiteral("did:jwk:PINNED");
        QCOMPARE(dependencyBlockerDetail(b),
                 QStringLiteral("not signed by the required key; requires did:jwk:PINNED"));
    }

    void a_satisfied_row_has_no_detail()
    {
        QCOMPARE(dependencyBlockerDetail(readDependencyBlocker(satisfiedRow())), QString());
    }

    // The payload QML renders.
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

    // A ternary here labels a signer mismatch "not_installed" on the way into
    // QML, and the dialog tells the user to install a package on disk.
    void the_wire_map_carries_the_signer_kind_and_both_dids()
    {
        const QVariantMap m = dependencyBlockerToMap(readDependencyBlocker(signerMismatchRow()));
        QCOMPARE(m.value("kind").toString(), QStringLiteral("signer_mismatch"));
        QCOMPARE(m.value("requiredSigner").toString(), QStringLiteral("did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6Il96N1dqMUd4RWhEdURHQ1hHdVJuOUdTdm1teHo3ZGtMY0dvaEdqMTJOMEUifQ"));
        QCOMPARE(m.value("signerDid").toString(),      QStringLiteral("did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IlFpT2tQMHJOZmtLSUtIZlFuME1OZjlabldINlFjc0NveFRvQjRfTmxJUDgifQ"));
        // Still on disk, so the row keeps the version it has.
        QCOMPARE(m.value("installedVersion").toString(), QStringLiteral("1.0.0"));
    }

    void the_wire_map_for_an_absent_dependency_reports_no_installed_version()
    {
        const QVariantMap m = dependencyBlockerToMap(readDependencyBlocker(absentRow()));
        QCOMPARE(m.value("kind").toString(), QStringLiteral("not_installed"));
        QVERIFY(m.value("installedVersion").toString().isEmpty());
    }

    // Choosing the headline: a set containing more than one kind must not claim
    // to be any one of them.
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

    // Sweeping unrecognised kinds into `absent` calls a pure signer set
    // "not installed".
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

    // Statuses this build deliberately admits: `cycle` because nothing here has
    // been driven against a real cyclic install, and an unrecognised status
    // because this gate is only an advisory pre-check in front of liblogos'
    // own resolver. Change either only with a run behind it.
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
