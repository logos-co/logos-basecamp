// srcdeps: AppsModel.cpp AppsFilterProxy.cpp InstallEnums.cpp
//
// Unit tests for AppsFilterProxy's filter chain, the welcome page's search.
// Lives in this test target alongside apps_model_test.cpp; both link the same
// AppsModel + AppsFilterProxy sources. These tests pin each filter behaviour
// directly.

#include "AppsFilterProxy.h"
#include "AppsModel.h"
#include "InstallEnums.h"

#include <QtTest/QtTest>
#include <QSignalSpy>

namespace {

// A catalog row whose newest version publishes exactly `variants` in its
// manifest's `main` map — the availability input.
QVariantMap variantRow(const QString& name,
                       const QString& repo,
                       const QStringList& variants)
{
    QVariantMap mainMap;
    for (const QString& v : variants) mainMap.insert(v, QStringLiteral("lib/e.so"));

    QVariantMap manifest;
    manifest.insert("version", QStringLiteral("1.0"));
    manifest.insert("main",    mainMap);

    QVariantMap entry;
    entry.insert("rootHash", QStringLiteral("H"));
    entry.insert("manifest", manifest);

    QVariantMap r;
    r.insert("name",          name);
    r.insert("repositoryUrl", repo);
    r.insert("type",          QStringLiteral("ui_qml"));
    r.insert("versions",      QVariantList{entry});
    return r;
}

QVariantMap row(const QString& name,
                const QString& repo,
                const QString& version,
                const QString& rootHash,
                const QString& type = "ui_qml")
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

    // ── filters compose AND-wise ───────────────────────────────────────
    void filters_compose_AND_not_OR()
    {
        m_model->replaceCatalog({
            row("wallet_ui",     "r", "1.0", "H1", "ui_qml"),
            row("wallet_module", "r", "1.0", "H2", "core"),
            row("chat_ui",       "r", "1.0", "H3", "ui_qml"),
        });
        m_proxy->setTypeFilter("ui_qml");
        m_proxy->setSearchText("wallet");
        QCOMPARE(m_proxy->rowCount(), 1);   // (wallet_ui, ui_qml)
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

    // ── unavailable rows ───────────────────────────────────────────────
    //
    // Availability is its own axis, AND-ed with the rest, and hides: catalogs
    // are published per OS/arch and build flavor, and a host the catalog
    // barely covers was otherwise shown mostly apps it can never install.

    void unavailable_hidesForeignPlatformRows()
    {
        m_model->setValidVariants({"linux-x86_64"});
        m_model->replaceCatalog({
            variantRow("mine",    "r", {"linux-x86_64"}),
            variantRow("foreign", "r", {"windows-x86_64"}),
        });
        QCOMPARE(m_proxy->visibleCount(), 1);
        QCOMPARE(nameAt(0), QStringLiteral("mine"));
    }

    // Installed rows are exempt whatever the catalog now says: a catalog that
    // drops our variant after the fact must not take the app's uninstall path
    // with it.
    void unavailable_neverHidesAnInstalledRow()
    {
        m_model->setValidVariants({"linux-x86_64"});
        m_model->replaceCatalog({variantRow("gone", "r", {"windows-x86_64"})});
        QCOMPARE(m_proxy->visibleCount(), 0);

        m_model->markInstalled("gone", "1.0", "H");
        QCOMPARE(m_proxy->visibleCount(), 1);
    }

    // A locally-installed app has NO catalog row, so it publishes no variants
    // and computes as unavailable. It must still be listed — this is the case
    // that broke every sideloaded app when the exemption was missing from the
    // grid/list delegates, leaving their tiles inert and unlaunchable.
    void unavailable_localOnlyInstalledAppStaysListed()
    {
        m_model->setValidVariants({"linux-x86_64"});
        QVariantMap installed;
        installed.insert("name",    QStringLiteral("sideloaded"));
        installed.insert("version", QStringLiteral("1.0"));
        installed.insert("type",    QStringLiteral("ui_qml"));
        // mergeLocalOnlyInstalled surfaces ONLY user-installed packages;
        // without this the row is never created and the test passes vacuously
        // against an empty model.
        installed.insert("installType", QStringLiteral("user"));
        m_model->mergeLocalOnlyInstalled({installed});

        QCOMPARE(m_proxy->visibleCount(), 1);
        QCOMPARE(nameAt(0), QStringLiteral("sideloaded"));
    }

    // The variant list lands on its own round trip, after the catalog. The
    // proxy must re-filter when it does, or the grid keeps showing rows the
    // model has since marked unavailable.
    void unavailable_refiltersWhenTheVariantListArrivesLate()
    {
        m_model->replaceCatalog({variantRow("foreign", "r", {"windows-x86_64"})});
        QCOMPARE(m_proxy->visibleCount(), 1);   // permissive until it answers

        m_model->setValidVariants({"linux-x86_64"});
        QCOMPARE(m_proxy->visibleCount(), 0);
    }

private:
    QString nameAt(int proxyRow) const
    {
        int nameRole = -1;
        const auto& roles = m_model->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            if (it.value() == "name") nameRole = it.key();
        return m_proxy->data(m_proxy->index(proxyRow, 0), nameRole).toString();
    }

    std::unique_ptr<AppsModel>       m_model;
    std::unique_ptr<AppsFilterProxy> m_proxy;
};

QTEST_GUILESS_MAIN(AppsFilterProxyTest)
#include "apps_filter_proxy_test.moc"
