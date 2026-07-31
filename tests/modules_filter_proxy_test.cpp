// srcdeps: ModulesFilterProxy.cpp ModuleInstanceModel.cpp
//
// Unit tests for ModulesFilterProxy — the QSortFilterProxyModel driving the
// Settings inspectors' search + sort. Constructed over a ModuleInstanceModel
// (not a mock) so the tests double as an integration check between the two.

#include "ModuleInstanceModel.h"
#include "ModulesFilterProxy.h"

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

QStringList names(const QAbstractItemModel& m)
{
    QStringList out;
    const int nameRole = [&]{
        const auto roles = m.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            if (it.value() == "name") return it.key();
        return -1;
    }();
    for (int i = 0; i < m.rowCount(); ++i)
        out.append(m.data(m.index(i, 0), nameRole).toString());
    return out;
}

} // namespace

class ModulesFilterProxyTest : public QObject {
    Q_OBJECT

private slots:
    // ── Passthrough when no filters ───────────────────────────────────
    void passthrough_when_no_filters()
    {
        ModuleInstanceModel src;
        src.replaceRows({ makeModule("a"), makeModule("b"), makeModule("c") });

        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        QCOMPARE(names(proxy), (QStringList{"a", "b", "c"}));
        QCOMPARE(proxy.visibleCount(), 3);
        QCOMPARE(proxy.totalCount(),   3);
    }

    // ── State filter ──────────────────────────────────────────────────
    void stateFilter_loaded_keeps_only_loaded_rows()
    {
        ModuleInstanceModel src;
        src.replaceRows({
            makeModule("off"),
            makeModule("on1", /*loaded=*/true),
            makeModule("on2", /*loaded=*/true),
        });

        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setStateFilter(QStringLiteral("loaded"));

        QCOMPARE(names(proxy), (QStringList{"on1", "on2"}));
        QCOMPARE(proxy.visibleCount(), 2);
        QCOMPARE(proxy.totalCount(),   3);   // unaffected by filter
    }

    void stateFilter_notLoaded_inverse()
    {
        ModuleInstanceModel src;
        src.replaceRows({
            makeModule("off"),
            makeModule("on", /*loaded=*/true),
        });
        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setStateFilter(QStringLiteral("notLoaded"));
        QCOMPARE(names(proxy), (QStringList{"off"}));
    }

    // ── Search ────────────────────────────────────────────────────────
    void search_matches_across_multiple_textual_roles()
    {
        ModuleInstanceModel src;
        src.replaceRows({
            makeModule("waku",   true, {{"description", "The transport"}}),
            makeModule("chat",   true, {{"description", "Waku-powered messaging"}}),
            makeModule("logger", true, {{"description", "Logging utilities"}}),
        });
        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setSearchText(QStringLiteral("waku"));
        // Matches "waku" by name AND "chat" by description ("Waku-powered ...").
        QCOMPARE(proxy.visibleCount(), 2);
    }

    void search_is_case_insensitive_and_trimmed()
    {
        ModuleInstanceModel src;
        src.replaceRows({ makeModule("Storage") });
        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setSearchText(QStringLiteral("   STORage  "));
        QCOMPARE(proxy.visibleCount(), 1);
    }

    void search_empty_accepts_everything()
    {
        ModuleInstanceModel src;
        src.replaceRows({ makeModule("a"), makeModule("b") });
        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setSearchText(QString());
        QCOMPARE(proxy.visibleCount(), 2);
    }

    // ── Sort ──────────────────────────────────────────────────────────
    void sort_by_label_ascending_by_default()
    {
        ModuleInstanceModel src;
        src.replaceRows({
            makeModule("charlie"),
            makeModule("alpha"),
            makeModule("bravo"),
        });
        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        QCOMPARE(names(proxy), (QStringList{"alpha", "bravo", "charlie"}));
    }

    void sort_by_numeric_role_uses_numeric_compare()
    {
        ModuleInstanceModel src;
        src.replaceRows({
            makeModule("a", true, {{"cpu", "9.5"}}),
            makeModule("b", true, {{"cpu", "10.5"}}),
            makeModule("c", true, {{"cpu", "1.0"}}),
        });
        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setSortRoleName(QStringLiteral("cpu"));
        // Numeric compare: 1.0 < 9.5 < 10.5 (not lexicographic "10.5" < "9.5")
        QCOMPARE(names(proxy), (QStringList{"c", "a", "b"}));
    }

    void sort_tiebreak_by_name_when_role_ties()
    {
        ModuleInstanceModel src;
        src.replaceRows({
            makeModule("zebra", true, {{"cpu", "5.0"}}),
            makeModule("alpha", true, {{"cpu", "5.0"}}),
            makeModule("mango", true, {{"cpu", "5.0"}}),
        });
        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        proxy.setSortRoleName(QStringLiteral("cpu"));
        // All same cpu → tiebreak by name so poll ticks don't shuffle rows.
        QCOMPARE(names(proxy), (QStringList{"alpha", "mango", "zebra"}));
    }

    // ── visibleCount / totalCount signals ─────────────────────────────
    // Header row-count depends on these firing when filters change.
    void visibleCount_signal_fires_on_filter_change()
    {
        ModuleInstanceModel src;
        src.replaceRows({
            makeModule("a", /*loaded=*/true),
            makeModule("b"),
        });
        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        QSignalSpy spy(&proxy, &ModulesFilterProxy::visibleCountChanged);
        proxy.setStateFilter(QStringLiteral("loaded"));
        QVERIFY(spy.count() >= 1);
        QCOMPARE(proxy.visibleCount(), 1);
    }

    void totalCount_signal_fires_on_source_change()
    {
        ModuleInstanceModel src;
        ModulesFilterProxy proxy;
        proxy.setSourceModel(&src);
        QSignalSpy spy(&proxy, &ModulesFilterProxy::totalCountChanged);
        src.replaceRows({ makeModule("a") });
        QVERIFY(spy.count() >= 1);
        QCOMPARE(proxy.totalCount(), 1);
    }

    // ── Sanity: setSourceModel(nullptr) is safe ──────────────────────
    void nullSourceModel_is_safe()
    {
        ModulesFilterProxy proxy;
        proxy.setSourceModel(nullptr);
        QCOMPARE(proxy.visibleCount(), 0);
        QCOMPARE(proxy.totalCount(),   0);
    }
};

QTEST_GUILESS_MAIN(ModulesFilterProxyTest)
#include "modules_filter_proxy_test.moc"
