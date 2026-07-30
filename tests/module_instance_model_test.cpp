// srcdeps: ModuleInstanceModel.cpp
//
// Unit tests for ModuleInstanceModel — the QAbstractListModel that adapts
// the QVariantList returned by uiModules()/coreModules() into named roles
// for the Settings inspectors. Same test harness as apps_model_test:
// plain QtTest, glob-discovered by tests/CMakeLists.txt.

#include "ModuleInstanceModel.h"

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest/QtTest>

namespace {

QVariantMap makeModule(const QString& name,
                       bool isLoaded = false,
                       const QVariantMap& extras = {})
{
    QVariantMap m;
    m.insert(QStringLiteral("name"),     name);
    m.insert(QStringLiteral("isLoaded"), isLoaded);
    for (auto it = extras.cbegin(); it != extras.cend(); ++it)
        m.insert(it.key(), it.value());
    return m;
}

int roleFor(const ModuleInstanceModel& model, const QByteArray& name)
{
    const auto roles = model.roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if (it.value() == name) return it.key();
    }
    return -1;
}

QVariant fieldFor(const ModuleInstanceModel& model,
                  int row,
                  const QByteArray& name)
{
    const int role = roleFor(model, name);
    Q_ASSERT(role >= 0);
    return model.data(model.index(row), role);
}

} // namespace

class ModuleInstanceModelTest : public QObject {
    Q_OBJECT

private slots:
    // ── roleNames contract ────────────────────────────────────────────
    // QML delegates read roles by name (`rowItem.label`, `rowItem.isLoaded`)
    // — renaming any of these silently breaks every inspector binding.
    void roleNames_stable()
    {
        ModuleInstanceModel model;
        const auto roles = model.roleNames();
        const QList<QByteArray> required{
            "name", "label", "description", "category", "type",
            "version", "iconPath", "installType",
            "isLoaded", "isMainUi", "hasMissingDeps",
            "statusText", "cpu", "memory",
        };
        QHash<QByteArray, bool> seen;
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            seen.insert(it.value(), true);
        for (const QByteArray& r : required)
            QVERIFY2(seen.contains(r), qPrintable("missing role: " + r));
    }

    // ── Basic population + role dispatch ──────────────────────────────
    void replaceRows_populates_normalised_fields()
    {
        ModuleInstanceModel model;
        model.replaceRows({
            makeModule("waku", /*loaded=*/true, {
                {"displayName", "Waku"},
                {"description", "The transport"},
                {"category",    "networking"},
                {"type",        "core"},
                {"version",     "1.2.3"},
                {"installType", "user"},
            }),
        });

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(fieldFor(model, 0, "name").toString(),        "waku");
        QCOMPARE(fieldFor(model, 0, "label").toString(),       "Waku");
        QCOMPARE(fieldFor(model, 0, "description").toString(), "The transport");
        QCOMPARE(fieldFor(model, 0, "category").toString(),    "networking");
        QCOMPARE(fieldFor(model, 0, "type").toString(),        "core");
        QCOMPARE(fieldFor(model, 0, "version").toString(),     "1.2.3");
        QCOMPARE(fieldFor(model, 0, "installType").toString(), "user");
        QCOMPARE(fieldFor(model, 0, "isLoaded").toBool(),      true);
        QCOMPARE(fieldFor(model, 0, "statusText").toString(),  "Loaded");
    }

    // displayName absent → label falls back to name (used everywhere QML
    // renders "displayName || name").
    void label_falls_back_to_name_when_displayName_missing()
    {
        ModuleInstanceModel model;
        model.replaceRows({ makeModule("no_display") });
        QCOMPARE(fieldFor(model, 0, "label").toString(), "no_display");
    }

    // ── statusText derivation ─────────────────────────────────────────
    void statusText_reflects_priority_mainUi_missingDeps_loaded()
    {
        ModuleInstanceModel model;
        model.replaceRows({
            makeModule("plain"),  // not loaded, not main, no missing
            makeModule("loaded",  /*loaded=*/true),
            makeModule("main",    /*loaded=*/true, {{"isMainUi", true}}),
            makeModule("broken",  /*loaded=*/true, {{"hasMissingDeps", true}}),
        });

        QCOMPARE(fieldFor(model, 0, "statusText").toString(), "Not loaded");
        QCOMPARE(fieldFor(model, 1, "statusText").toString(), "Loaded");
        // Main UI wins over Loaded — the badge should read "Main UI" for main_ui.
        QCOMPARE(fieldFor(model, 2, "statusText").toString(), "Main UI");
        // Missing deps wins over Loaded (blocks the module regardless of load state).
        QCOMPARE(fieldFor(model, 3, "statusText").toString(), "Missing deps");
    }

    // ── Numeric coercion ──────────────────────────────────────────────
    // Core stats arrive as either doubles or strings depending on what the
    // module reported. The proxy sorts numerically — coerce up front so the
    // roles are always numeric.
    void numeric_stats_coerced_from_string()
    {
        ModuleInstanceModel model;
        model.replaceRows({
            makeModule("mod", /*loaded=*/true, {
                {"cpu",    "12.5"},
                {"memory", "48.0"},
            }),
        });
        QCOMPARE(fieldFor(model, 0, "cpu").toDouble(),    12.5);
        QCOMPARE(fieldFor(model, 0, "memory").toDouble(), 48.0);
    }

    void numeric_stats_default_zero_when_unparseable()
    {
        ModuleInstanceModel model;
        model.replaceRows({
            makeModule("mod", /*loaded=*/true, {{"cpu", "nope"}}),
        });
        QCOMPARE(fieldFor(model, 0, "cpu").toDouble(), 0.0);
    }

    // ── Empty rows / skipping ─────────────────────────────────────────
    void empty_name_is_skipped()
    {
        ModuleInstanceModel model;
        QVariantMap noName; noName.insert("isLoaded", true);
        model.replaceRows({ noName, makeModule("real") });
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(fieldFor(model, 0, "name").toString(), "real");
    }

    void replaceRows_empty_clears_model()
    {
        ModuleInstanceModel model;
        model.replaceRows({ makeModule("a"), makeModule("b") });
        QCOMPARE(model.rowCount(), 2);
        model.replaceRows({});
        QCOMPARE(model.rowCount(), 0);
    }

    // ── Patch-in-place fast path ──────────────────────────────────────
    // The 2s core-stats poll re-runs replaceRows with the same rows in the
    // same order, only CPU/memory changed. If we reset the model every tick,
    // the table's scroll position resets and every delegate rebuilds — a
    // visible flicker. The patch path is the fix; verify it emits
    // dataChanged on the moving roles only (not modelReset).
    void patchInPlace_when_names_stable_emits_dataChanged_only()
    {
        ModuleInstanceModel model;
        model.replaceRows({
            makeModule("a", true, {{"cpu", "1.0"}, {"memory", "10.0"}}),
            makeModule("b", true, {{"cpu", "2.0"}, {"memory", "20.0"}}),
        });

        QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

        // Same identity + order, only stats changed.
        model.replaceRows({
            makeModule("a", true, {{"cpu", "1.5"}, {"memory", "10.0"}}),
            makeModule("b", true, {{"cpu", "2.0"}, {"memory", "25.0"}}),
        });

        QCOMPARE(resetSpy.count(), 0);
        // One dataChanged per changed row.
        QCOMPARE(dataSpy.count(), 2);
        QCOMPARE(fieldFor(model, 0, "cpu").toDouble(),    1.5);
        QCOMPARE(fieldFor(model, 1, "memory").toDouble(), 25.0);
    }

    // Identical input on a re-tick with zero changes: no signals at all.
    // Prevents QML delegates from re-rendering for nothing.
    void patchInPlace_idempotent_zero_signals_on_identical_input()
    {
        ModuleInstanceModel model;
        model.replaceRows({ makeModule("a", true, {{"cpu", "1.0"}}) });

        QSignalSpy dataSpy(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        model.replaceRows({ makeModule("a", true, {{"cpu", "1.0"}}) });
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(dataSpy.count(),  0);
    }

    // Row identity change (add/remove/reorder) falls back to a full reset.
    void identityChange_triggers_modelReset()
    {
        ModuleInstanceModel model;
        model.replaceRows({ makeModule("a"), makeModule("b") });

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        // A different row added at position 0 — order changed.
        model.replaceRows({ makeModule("z"), makeModule("a"), makeModule("b") });
        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(model.rowCount(), 3);
    }

    // ── Boolean state derivations ─────────────────────────────────────
    void isLoaded_reflects_input()
    {
        ModuleInstanceModel model;
        model.replaceRows({
            makeModule("off", /*loaded=*/false),
            makeModule("on",  /*loaded=*/true),
        });
        QCOMPARE(fieldFor(model, 0, "isLoaded").toBool(), false);
        QCOMPARE(fieldFor(model, 1, "isLoaded").toBool(), true);
    }
};

QTEST_GUILESS_MAIN(ModuleInstanceModelTest)
#include "module_instance_model_test.moc"
