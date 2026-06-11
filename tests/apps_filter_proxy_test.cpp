// Unit tests for AppsFilterProxy's filter chain. Lives in this test target
// alongside apps_model_test.cpp; both link the same AppsModel + AppsFilterProxy
// sources. The filter proxy was previously only exercised indirectly through
// QML, which let the multi-repo / case-sensitivity / install-state edge cases
// drift without warning. These tests pin each filter behaviour directly.

#include "AppsFilterProxy.h"
#include "AppsModel.h"
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

class AppsFilterProxyTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_model = std::make_unique<AppsModel>();
        m_proxy = std::make_unique<AppsFilterProxy>();
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
        QCOMPARE(m_proxy->data(mi, AppsModel::RepositoryUrlRole).toString(),
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
        QSignalSpy spy(m_proxy.get(), &AppsFilterProxy::visibleCountChanged);
        m_proxy->setSearchText("a");
        QVERIFY(spy.count() >= 1);
        QCOMPARE(m_proxy->visibleCount(), 1);
    }

private:
    std::unique_ptr<AppsModel>       m_model;
    std::unique_ptr<AppsFilterProxy> m_proxy;
};

QTEST_GUILESS_MAIN(AppsFilterProxyTest)
#include "apps_filter_proxy_test.moc"
