// srcdeps: UninstallPlan.cpp
#include <QtTest/QtTest>

#include "UninstallPlan.h"

using namespace uninstallplan;

namespace {

// The worked example from the plan:
//
//   Chat ─► chat_module ─► waku_module ─► libp2p_module
//                └───────► accounts_module   (embedded)
//   Wallet ──────────────────────────────► libp2p_module
//
// `dependencies` holds RECURSIVE forward closures (that's what
// resolveFlatDependencies returns), so each entry lists everything reachable,
// not just direct edges.
Input worldChatAndWallet()
{
    Input in;
    in.installed = {
        "Chat", "chat_module", "waku_module", "libp2p_module",
        "accounts_module", "Wallet",
    };
    in.dependencies = {
        {"Chat",         {"chat_module", "waku_module", "libp2p_module", "accounts_module"}},
        {"chat_module",  {"waku_module", "libp2p_module", "accounts_module"}},
        {"waku_module",  {"libp2p_module"}},
        {"Wallet",       {"libp2p_module"}},
    };
    in.dependents = {
        {"chat_module",    {"Chat"}},
        {"waku_module",    {"chat_module", "Chat"}},
        {"libp2p_module",  {"waku_module", "chat_module", "Chat", "Wallet"}},
        {"accounts_module",{"chat_module", "Chat"}},
    };
    in.embedded = {"accounts_module"};
    in.protectedNames = {"main_ui"};
    return in;
}

QStringList keptNames(const Plan& p)
{
    QStringList out;
    for (const KeptRow& k : p.kept) out << k.name;
    return out;
}

QStringList dependentNames(const Plan& p)
{
    QStringList out;
    for (const Row& r : p.dependents) out << r.name;
    return out;
}

KeptReason reasonFor(const Plan& p, const QString& name)
{
    for (const KeptRow& k : p.kept)
        if (k.name == name) return k.reason;
    // No sentinel value in the enum; the tests that call this always assert
    // the name is present, so returning Unused here is just a fall-through.
    return KeptReason::Unused;
}

QStringList requiredByFor(const Plan& p, const QString& name)
{
    for (const KeptRow& k : p.kept)
        if (k.name == name) return k.requiredBy;
    return {};
}

}  // namespace

class UninstallTest : public QObject {
    Q_OBJECT
private slots:

    // ── The two headline cases from the plan's worked example ──────────

    void nothing_depends_on_target_batches_its_private_deps()
    {
        Input in = worldChatAndWallet();
        in.targets = {"Chat"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.batch, (QStringList{"Chat", "chat_module", "waku_module"}));
        // libp2p_module is still Wallet's; accounts_module ships in the bundle.
        QCOMPARE(keptNames(p), (QStringList{"libp2p_module", "accounts_module"}));
        QCOMPARE(reasonFor(p, "libp2p_module"),   KeptReason::RequiredBy);
        QCOMPARE(requiredByFor(p, "libp2p_module"), (QStringList{"Wallet"}));
        QCOMPARE(reasonFor(p, "accounts_module"), KeptReason::Embedded);
        QVERIFY(p.dependents.isEmpty());
    }

    void one_installed_dependent_collapses_the_cleanup_to_the_target()
    {
        Input in = worldChatAndWallet();
        in.installed << "irc_ui" << "delivery_demo";
        // Both hang off Chat; irc_ui also pulls chat_module + waku_module in
        // its own right, which is what keeps them alive.
        in.dependencies.insert("irc_ui",
            {"Chat", "chat_module", "waku_module", "libp2p_module", "accounts_module"});
        in.dependencies.insert("delivery_demo", {"Chat"});
        in.dependents.insert("Chat", {"irc_ui", "delivery_demo"});
        in.targets = {"Chat"};
        in.loaded  = {"irc_ui"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.batch, (QStringList{"Chat"}));
        QCOMPARE(keptNames(p),
                 (QStringList{"chat_module", "waku_module", "libp2p_module",
                              "accounts_module"}));
        QCOMPARE(reasonFor(p, "chat_module"), KeptReason::RequiredBy);
        QCOMPARE(requiredByFor(p, "chat_module"), (QStringList{"irc_ui"}));
        QCOMPARE(dependentNames(p), (QStringList{"irc_ui", "delivery_demo"}));
        // Loaded flag drives the popup's "(running — unloaded now)" suffix.
        QCOMPARE(p.dependents.at(0).isLoaded, true);
        QCOMPARE(p.dependents.at(1).isLoaded, false);
    }

    // ── Scenario 12 — dep shared with another installed app ────────────

    void shared_dep_is_kept_and_names_every_holder()
    {
        Input in = worldChatAndWallet();
        in.installed << "Notes";
        in.dependencies.insert("Notes", {"libp2p_module"});
        in.targets = {"Chat"};

        const Plan p = composeFrom(in);

        QCOMPARE(reasonFor(p, "libp2p_module"), KeptReason::RequiredBy);
        // Order follows `installed`, so the list is stable across runs.
        QCOMPARE(requiredByFor(p, "libp2p_module"), (QStringList{"Wallet", "Notes"}));
    }

    // ── Scenario 13 — dep held only by a breaking dependent ────────────
    //
    // Open decision 1: we keep it. The dependent is about to break either
    // way, but removing what it needs makes the breakage unrecoverable
    // without a reinstall.

    void dep_held_only_by_a_breaking_dependent_is_kept()
    {
        Input in;
        in.installed    = {"Chat", "chat_module", "irc_ui"};
        in.dependencies = {
            {"Chat",        {"chat_module"}},
            {"irc_ui",      {"Chat", "chat_module"}},
        };
        in.dependents = {
            {"Chat",        {"irc_ui"}},
            {"chat_module", {"Chat", "irc_ui"}},
        };
        in.targets       = {"Chat"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.batch, (QStringList{"Chat"}));
        QCOMPARE(reasonFor(p, "chat_module"), KeptReason::RequiredBy);
        QCOMPARE(requiredByFor(p, "chat_module"), (QStringList{"irc_ui"}));
        QCOMPARE(dependentNames(p), (QStringList{"irc_ui"}));
    }

    // ── Scenario 14 — embedded dep in the closure ──────────────────────

    void embedded_dep_is_kept_and_filtered_out_of_the_batch()
    {
        Input in = worldChatAndWallet();
        in.targets = {"Chat"};

        const Plan p = composeFrom(in);

        QVERIFY(!p.batch.contains("accounts_module"));
        QCOMPARE(reasonFor(p, "accounts_module"), KeptReason::Embedded);
    }

    void protected_dep_is_kept_with_reason_protected()
    {
        Input in;
        in.installed       = {"Chat", "main_ui"};
        in.dependencies    = {{"Chat", {"main_ui"}}};
        in.protectedNames  = {"main_ui"};
        in.targets         = {"Chat"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.batch, (QStringList{"Chat"}));
        // Distinct from Embedded — the popup renders these under different
        // labels so the user knows *why* the package is un-removable
        // ("built in" vs "required by the app").
        QCOMPARE(reasonFor(p, "main_ui"), KeptReason::Protected);
    }

    // Both Embedded and Protected packages are filtered out of the batch
    // and land in Kept, but with distinct reasons. Regression guard against
    // the old label-collapse bug where protected was mislabelled Embedded.
    void embedded_and_protected_produce_distinct_reasons()
    {
        Input in;
        in.installed      = {"App", "core_lib", "shipped_lib", "main_ui"};
        in.dependencies   = {{"App", {"core_lib", "shipped_lib", "main_ui"}}};
        in.embedded       = {"shipped_lib"};
        in.protectedNames = {"main_ui"};
        in.targets        = {"App"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.batch, (QStringList{"App", "core_lib"}));
        QCOMPARE(reasonFor(p, "shipped_lib"), KeptReason::Embedded);
        QCOMPARE(reasonFor(p, "main_ui"),     KeptReason::Protected);
    }

    // ── Scenario 15 — dep in a manifest but not installed ──────────────

    void dep_not_on_disk_is_excluded_from_candidates_and_from_the_popup()
    {
        Input in;
        in.installed     = {"Chat", "chat_module"};
        in.dependencies  = {{"Chat", {"chat_module", "ghost_module"}}};
        in.targets       = {"Chat"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.batch, (QStringList{"Chat", "chat_module"}));
        QVERIFY(!keptNames(p).contains("ghost_module"));
    }

    // ── Scenario 16 — dependency cycle ─────────────────────────────────

    void cycles_terminate_and_dedupe_by_name()
    {
        Input in;
        in.installed    = {"a", "b", "c"};
        // a ↔ b, both reach c. Flat closures already contain the cycle.
        in.dependencies = {
            {"a", {"b", "c", "a"}},
            {"b", {"a", "c", "b"}},
            {"c", {}},
        };
        in.dependents = {
            {"a", {"b"}},
            {"b", {"a"}},
            {"c", {"a", "b"}},
        };
        in.targets       = {"a"};

        const Plan p = composeFrom(in);

        // "a" appears in its own closure; it must not be listed twice.
        QCOMPARE(p.batch, (QStringList{"a", "b", "c"}));
        QCOMPARE(p.removable.size(), 3);
        QVERIFY(p.kept.isEmpty());
        // b is inside the closure, so it isn't warned about as a dependent.
        QVERIFY(p.dependents.isEmpty());
    }

    void duplicate_targets_are_deduped()
    {
        Input in = worldChatAndWallet();
        in.targets = {"Chat", "Chat", "chat_module"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.batch, (QStringList{"Chat", "chat_module", "waku_module"}));
        QCOMPARE(p.removable.at(0).isTarget, true);
        QCOMPARE(p.removable.at(1).isTarget, true);
        // waku_module came in as an orphan, not as something the user picked.
        QCOMPARE(p.removable.at(2).isTarget, false);
    }

    // ── Scenario 18 — uninstall before the first refresh completes ─────
    //
    // Empty caches must degrade to "target only", never to a wrong batch.
    // The menu item is gated on the caches being populated; this pins the
    // fallback in case it ever slips through.

    void empty_caches_degrade_to_the_target_alone()
    {
        Input in;
        in.installed     = {"Chat"};
        in.targets       = {"Chat"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.batch, (QStringList{"Chat"}));
        QVERIFY(p.kept.isEmpty());
        QVERIFY(p.dependents.isEmpty());
    }

    void target_that_is_not_installed_is_dropped()
    {
        Input in = worldChatAndWallet();
        in.targets = {"Chat", "never_installed"};

        const Plan p = composeFrom(in);

        QVERIFY(!p.batch.contains("never_installed"));
        QCOMPARE(p.batch.first(), QStringLiteral("Chat"));
    }

    // ── explainOf: batch is already fixed, narrate over the same graph ─

    void explain_pass_keeps_the_batch_exactly_as_given()
    {
        Input in = worldChatAndWallet();
        // Explain pass: targets ARE the batch the module handed back.
        in.targets = {"Chat", "chat_module", "waku_module"};

        const Plan p = explainOf(in);

        QCOMPARE(p.batch, (QStringList{"Chat", "chat_module", "waku_module"}));
        QCOMPARE(keptNames(p), (QStringList{"libp2p_module", "accounts_module"}));
        QCOMPARE(reasonFor(p, "libp2p_module"), KeptReason::RequiredBy);
    }

    // Scenario 19 — a PMUI/Settings-initiated batch runs through explainOf,
    // so a now-unneeded dep shows up as a cleanup hint ("unused") rather
    // than being silently swept into someone else's batch.
    void pmui_style_batch_reports_orphans_as_unused_without_removing_them()
    {
        Input in = worldChatAndWallet();
        in.targets = {"Chat"};

        const Plan p = explainOf(in);

        QCOMPARE(p.batch, (QStringList{"Chat"}));
        QCOMPARE(reasonFor(p, "chat_module"), KeptReason::Unused);
        QCOMPARE(reasonFor(p, "waku_module"), KeptReason::Unused);
        QCOMPARE(reasonFor(p, "libp2p_module"), KeptReason::RequiredBy);
        QCOMPARE(reasonFor(p, "accounts_module"), KeptReason::Embedded);
    }

    // ── Row decoration ─────────────────────────────────────────────────

    void rows_carry_display_name_version_and_loaded_state()
    {
        Input in = worldChatAndWallet();
        in.targets      = {"Chat"};
        in.displayNames = {{"Chat", "Chat"}, {"chat_module", "Chat Module"}};
        in.versions     = {{"Chat", "1.2.0"}, {"chat_module", "0.9.1"},
                           {"waku_module", "0.4.0"}};
        in.loaded       = {"Chat", "chat_module"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.removable.at(0).displayName, QStringLiteral("Chat"));
        QCOMPARE(p.removable.at(0).version,     QStringLiteral("1.2.0"));
        QCOMPARE(p.removable.at(0).isTarget,    true);
        QCOMPARE(p.removable.at(0).isLoaded,    true);
        QCOMPARE(p.removable.at(1).displayName, QStringLiteral("Chat Module"));
        QCOMPARE(p.removable.at(1).isTarget,    false);
        // No displayName entry → falls back to the raw name.
        QCOMPARE(p.removable.at(2).displayName, QStringLiteral("waku_module"));
        QCOMPARE(p.removable.at(2).isLoaded,    false);
    }

    void empty_and_whitespace_targets_are_ignored()
    {
        Input in = worldChatAndWallet();
        in.targets = {"", "Chat"};

        const Plan p = composeFrom(in);

        QCOMPARE(p.batch, (QStringList{"Chat", "chat_module", "waku_module"}));
    }

    void no_targets_yields_an_empty_plan()
    {
        Input in = worldChatAndWallet();
        in.targets = {};

        const Plan p = composeFrom(in);

        QVERIFY(p.batch.isEmpty());
        QVERIFY(p.removable.isEmpty());
        QVERIFY(p.kept.isEmpty());
        QVERIFY(p.dependents.isEmpty());
    }
};

QTEST_GUILESS_MAIN(UninstallTest)
#include "uninstall_test.moc"
