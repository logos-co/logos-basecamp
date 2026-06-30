// Unit tests for ModuleFilterProxy's filter chain. Lives in this test target
// alongside module_model_test.cpp; both link the same ModuleModel + ModuleFilterProxy
// sources. The filter proxy was previously only exercised indirectly through
// QML, which let the multi-repo / case-sensitivity / install-state edge cases
// drift without warning. These tests pin each filter behaviour directly.

#include "ModuleFilterProxy.h"
#include "ModuleModel.h"
#include "InstallEnums.h"

#include <QtTest/QtTest>
#include <QSignalSpy>

namespace {

QVariantMap row(const QString& name,
                const QString& repo,
                const QString& version,
                const QString& rootHash,
                const QString& type = "ui_qml",
                const QString& category = "")
{
    QVariantMap manifest;
    manifest.insert("version", version);

    QVariantMap entry;
    entry.insert("rootHash", rootHash);
    entry.insert("manifest", manifest);

    QVariantMap r;
    r.insert("name",          name);
    r.insert("repositoryUrl", repo);
    r.insert("type",          type);
    r.insert("category",      category);
    r.insert("versions",      QVariantList{ entry });
    return r;
}

} // namespace

class ModuleFilterProxyTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_model = std::make_unique<ModuleModel>();
        m_proxy = std::make_unique<ModuleFilterProxy>();
        m_proxy->setSourceModel(m_model.get());
        // Production default is excludeMainUi=true; turn off here so the
        // fixture's rows aren't accidentally hidden.
        m_proxy->setExcludeMainUi(false);
    }

    // ── typeFilter ─────────────────────────────────────────────────────
    void typeFilter_passes_only_matching_type()
    {
        m_model->replaceCatalog({
            row("ui_a", "r", "1.0", "H1", "ui_qml"),
            row("core_b", "r", "1.0", "H2", "core"),
            row("ui_c", "r", "1.0", "H3", "ui_qml"),
        });
        QCOMPARE(m_proxy->rowCount(), 3);
        m_proxy->setTypeFilter("ui_qml");
        QCOMPARE(m_proxy->rowCount(), 2);
        m_proxy->setTypeFilter("core");
        QCOMPARE(m_proxy->rowCount(), 1);
        m_proxy->setTypeFilter("");
        QCOMPARE(m_proxy->rowCount(), 3);
    }

    // ── categoryFilter ─────────────────────────────────────────────────
    void categoryFilter_case_insensitive_with_first_letter_capitalised()
    {
        m_model->replaceCatalog({
            row("a", "r", "1.0", "H1", "ui_qml", "wallet"),
            row("b", "r", "1.0", "H2", "ui_qml", "Wallet"),  // capitalised
            row("c", "r", "1.0", "H3", "ui_qml", "chat"),
        });
        // Implementation capitalises first letter before comparing, so the
        // filter value "Wallet" matches both lower- and upper-case rows.
        m_proxy->setCategoryFilter("Wallet");
        QCOMPARE(m_proxy->rowCount(), 2);
        // "All" + "" both mean no filter.
        m_proxy->setCategoryFilter("All");
        QCOMPARE(m_proxy->rowCount(), 3);
        m_proxy->setCategoryFilter("");
        QCOMPARE(m_proxy->rowCount(), 3);
    }

    // ── searchText ─────────────────────────────────────────────────────
    void searchText_case_insensitive_substring_on_name()
    {
        m_model->replaceCatalog({
            row("Wallet_UI", "r", "1.0", "H1"),
            row("chat_module", "r", "1.0", "H2"),
            row("wallet_module", "r", "1.0", "H3"),
        });
        m_proxy->setSearchText("wallet");
        QCOMPARE(m_proxy->rowCount(), 2);
        m_proxy->setSearchText("MODULE");
        QCOMPARE(m_proxy->rowCount(), 2);
        m_proxy->setSearchText("");
        QCOMPARE(m_proxy->rowCount(), 3);
    }

    // ── installStateFilter ─────────────────────────────────────────────
    void installStateFilter_installed_versus_notInstalled()
    {
        m_model->replaceCatalog({
            row("a", "r", "1.0", "H1"),
            row("b", "r", "1.0", "H2"),
        });
        m_model->markInstalled("a", "1.0", "H1");

        m_proxy->setInstallStateFilter("installed");
        QCOMPARE(m_proxy->rowCount(), 1);
        m_proxy->setInstallStateFilter("notInstalled");
        QCOMPARE(m_proxy->rowCount(), 1);
        m_proxy->setInstallStateFilter("all");
        QCOMPARE(m_proxy->rowCount(), 2);
    }

    // ── excludeMainUi ──────────────────────────────────────────────────
    void excludeMainUi_hides_only_the_main_ui_row()
    {
        m_model->replaceCatalog({
            row("main_ui", "r", "1.0", "H1"),
            row("wallet_ui", "r", "1.0", "H2"),
        });
        m_proxy->setExcludeMainUi(true);
        QCOMPARE(m_proxy->rowCount(), 1);
        m_proxy->setExcludeMainUi(false);
        QCOMPARE(m_proxy->rowCount(), 2);
    }

    // ── repositoryUrlFilter ────────────────────────────────────────────
    void repositoryUrlFilter_pins_to_a_single_repo()
    {
        m_model->replaceCatalog({
            row("wallet_ui", "https://repo1", "1.0", "H1"),
            row("wallet_ui", "https://repo2", "1.0", "H2"),
            row("chat_ui",   "https://repo1", "1.0", "H3"),
        });
        m_proxy->setRepositoryUrlFilter("https://repo1");
        QCOMPARE(m_proxy->rowCount(), 2);
        m_proxy->setRepositoryUrlFilter("https://repo2");
        QCOMPARE(m_proxy->rowCount(), 1);
        m_proxy->setRepositoryUrlFilter("");
        QCOMPARE(m_proxy->rowCount(), 3);
    }

    // ── requiredPackages ───────────────────────────────────────────────
    // Locks the (name, repo) pinning contract used by the Add Application
    // dialog. An empty repo on an entry means "any repo's row for this
    // name passes" (resolver-fallback); a non-empty repo pins to exactly
    // that (repo, name) row.
    void requiredPackages_pinned_repo_matches_exact_pair()
    {
        m_model->replaceCatalog({
            row("wallet_ui", "https://repo1", "1.0", "H1"),
            row("wallet_ui", "https://repo2", "1.0", "H2"),
        });
        m_proxy->setRequiredPackages({
            QVariantMap{ {"name", "wallet_ui"},
                         {"repositoryUrl", "https://repo2"} },
        });
        QCOMPARE(m_proxy->rowCount(), 1);
        // It really is the repo2 row, not repo1.
        const QModelIndex mi = m_proxy->index(0, 0);
        QCOMPARE(m_proxy->data(mi, ModuleModel::RepositoryUrlRole).toString(),
                 QStringLiteral("https://repo2"));
    }

    void requiredPackages_empty_repo_matches_any_repo()
    {
        m_model->replaceCatalog({
            row("wallet_ui", "https://repo1", "1.0", "H1"),
            row("wallet_ui", "https://repo2", "1.0", "H2"),
        });
        m_proxy->setRequiredPackages({
            QVariantMap{ {"name", "wallet_ui"}, {"repositoryUrl", ""} },
        });
        QCOMPARE(m_proxy->rowCount(), 2);
    }

    // ── filters compose AND-wise ───────────────────────────────────────
    void filters_compose_AND_not_OR()
    {
        m_model->replaceCatalog({
            row("wallet_ui", "https://repo1", "1.0", "H1", "ui_qml", "wallet"),
            row("wallet_ui", "https://repo2", "1.0", "H2", "ui_qml", "wallet"),
            row("chat_ui",   "https://repo1", "1.0", "H3", "ui_qml", "chat"),
        });
        m_proxy->setTypeFilter("ui_qml");
        m_proxy->setCategoryFilter("Wallet");
        m_proxy->setRepositoryUrlFilter("https://repo1");
        QCOMPARE(m_proxy->rowCount(), 1);   // (wallet_ui, repo1, wallet)
    }

    // ── BUG-013: search must match the VISIBLE fields, not just name ──
    // Users type what they see (display name / description), but the proxy
    // only searched the internal package name, filtering out rows whose
    // visible title or description matched. These pin the fix.
    void searchText_matches_displayName()
    {
        QVariantMap r = row("wallet_ui", "r", "1.0", "H1");
        r.insert("displayName", "My Wallet");
        r.insert("description", "Manage your funds");
        m_model->replaceCatalog({ r });
        QCOMPARE(m_proxy->rowCount(), 1);

        // "Wallet" appears in the display name but the internal name is
        // "wallet_ui" — the old code matched it only by luck of the substring;
        // use a display-only token to prove display-name search works.
        m_proxy->setSearchText("My Wal");
        QCOMPARE(m_proxy->rowCount(), 1);   // FAILS before the fix (name has no "My Wal")
        m_proxy->setSearchText("");
    }

    void searchText_matches_description()
    {
        QVariantMap r = row("soulseek_ui", "r", "1.0", "H1");
        r.insert("displayName", "Soulseek");
        r.insert("description", "Peer to peer file sharing");
        m_model->replaceCatalog({ r });
        QCOMPARE(m_proxy->rowCount(), 1);

        m_proxy->setSearchText("file sharing");
        QCOMPARE(m_proxy->rowCount(), 1);   // FAILS before the fix
        // A token in none of name/display/description still excludes the row.
        m_proxy->setSearchText("nonexistent-zzz");
        QCOMPARE(m_proxy->rowCount(), 0);
        m_proxy->setSearchText("");
    }

    void searchText_still_matches_internal_name()
    {
        // Regression guard: internal-name search must keep working.
        QVariantMap r = row("waku_module", "r", "1.0", "H1");
        m_model->replaceCatalog({ r });
        m_proxy->setSearchText("waku");
        QCOMPARE(m_proxy->rowCount(), 1);
        m_proxy->setSearchText("");
    }

    void isLoadedFilter_splits_loaded_and_unloaded()
    {
        m_model->replaceCatalog({
            row("loaded_app", "r", "1.0", "H1", "ui_qml"),
            row("fresh_app", "r", "1.0", "H2", "ui_qml"),
        });
        m_model->markInstalled("loaded_app", "1.0", "H1");
        m_model->markInstalled("fresh_app", "1.0", "H2");
        m_model->seedInstalledOnly(QStringLiteral("loaded_app"), QStringLiteral("ui_qml"), {});
        m_model->seedInstalledOnly(QStringLiteral("fresh_app"), QStringLiteral("ui_qml"), {});
        m_model->setRoleByName("loaded_app", ModuleModel::IsLoadedRole, true);
        m_proxy->setRequireUiPluginRecord(true);

        m_proxy->setIsLoadedFilter(1);
        QCOMPARE(m_proxy->rowCount(), 1);
        QCOMPARE(m_proxy->data(m_proxy->index(0, 0), ModuleModel::NameRole).toString(),
                 QStringLiteral("loaded_app"));

        m_proxy->setIsLoadedFilter(0);
        QCOMPARE(m_proxy->rowCount(), 1);
        QCOMPARE(m_proxy->data(m_proxy->index(0, 0), ModuleModel::NameRole).toString(),
                 QStringLiteral("fresh_app"));
    }

    // Regression: toggling IsLoadedRole on an existing row must move it
    // across the isLoadedFilter=1 / =0 partitions. QSortFilterProxyModel
    // only re-evaluates filterAcceptsRow on dataChanged when filterRole()
    // is in the emitted roles list; the launcher proxies filter on custom
    // roles, so without an explicit invalidateRowsFilter() the sidebar
    // tile stayed in the "loaded" section after the user closed the app.
    void isLoadedFilter_reevaluates_on_role_change()
    {
        m_model->replaceCatalog({
            row("app_a", "r", "1.0", "H1", "ui_qml"),
        });
        m_model->markInstalled("app_a", "1.0", "H1");
        m_model->seedInstalledOnly("app_a", "ui_qml", {});
        m_model->setRoleByName("app_a", ModuleModel::IsLoadedRole, true);
        m_proxy->setRequireUiPluginRecord(true);
        m_proxy->setIsLoadedFilter(1);

        QCOMPARE(m_proxy->rowCount(), 1);

        // Simulate UIPluginManager::updateUiPluginLoadedState(false)
        // after onPluginWindowClosed.
        m_model->setRoleByName("app_a", ModuleModel::IsLoadedRole, false);

        QCOMPARE(m_proxy->rowCount(), 0);

        // And it must show up in the unloaded proxy.
        ModuleFilterProxy unloaded;
        unloaded.setSourceModel(m_model.get());
        unloaded.setExcludeMainUi(false);
        unloaded.setRequireUiPluginRecord(true);
        unloaded.setIsLoadedFilter(0);
        QCOMPARE(unloaded.rowCount(), 1);
        QCOMPARE(unloaded.data(unloaded.index(0, 0), ModuleModel::NameRole).toString(),
                 QStringLiteral("app_a"));
    }

    // ── visibleCount stays in sync with rowCount ──────────────────────
    // QML bindings consume visibleCount; if it drifts the "Loading…"
    // collapse / "(N)" badge breaks. The proxy emits visibleCountChanged
    // on every row delta.
    void visibleCount_change_signal_fires_on_filter_change()
    {
        m_model->replaceCatalog({
            row("a", "r", "1.0", "H1"),
            row("b", "r", "1.0", "H2"),
        });
        QSignalSpy spy(m_proxy.get(), &ModuleFilterProxy::visibleCountChanged);
        m_proxy->setSearchText("a");
        QVERIFY(spy.count() >= 1);
        QCOMPARE(m_proxy->visibleCount(), 1);
    }

private:
    std::unique_ptr<ModuleModel>       m_model;
    std::unique_ptr<ModuleFilterProxy> m_proxy;
};

QTEST_GUILESS_MAIN(ModuleFilterProxyTest)
#include "module_filter_proxy_test.moc"
