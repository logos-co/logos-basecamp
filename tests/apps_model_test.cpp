// srcdeps: AppsModel.cpp InstallEnums.cpp InstallRegistry.cpp
//
// Unit tests for AppsModel's per-row install-status compute. Built and
// driven the same way the sandbox-test is — plain QtTest, standalone
// CMakeLists in this directory, run via `nix build .#unit-tests`. No
// logos-test-framework dependency: AppsModel is a pure Qt model, not a
// Logos module.
//
// What's tested: recomputeInstallStatus + markInstalled + setMissingDeps
// + replaceCatalog via the public model API. No IPC, no display, no
// PackageCoordinator. Run locally:
//
//   nix build .#unit-tests -L

#include "AppsModel.h"
#include "InstallEnums.h"
#include "InstallRegistry.h"

#include <QtTest/QtTest>
#include <QVariantList>
#include <QVariantMap>

namespace {

// ── DSL: small builders so each test reads like its scenario ──────────────

// Bridge to the pre-InstallRegistry AppsModel API. Older tests called
// model.setInstallStage(name, stage[, error]); today AppsModel reads stage
// via a separate InstallRegistry. These helpers keep the tests readable
// without touching the InstallRegistry contract line-by-line:
//   - `stage(reg, name, InstallStage::Installing)` — begin() + setStage()
//   - `stageFailed(reg, name, err)`                — begin() + fail()
// InstallRegistry::setStage/fail no-op if the name isn't already begun,
// so begin() first (using empty target strings — not part of the assertion).
inline void stage(InstallRegistry& reg,
                  const QString& name,
                  InstallStage::Value s)
{
    reg.begin(name, /*targetVersion=*/QString(),
                    /*targetHash=*/QString(),
                    /*startedByTopLevel=*/name);
    reg.setStage(name, s);
}

inline void stageFailed(InstallRegistry& reg,
                        const QString& name,
                        const QString& error)
{
    reg.begin(name, /*targetVersion=*/QString(),
                    /*targetHash=*/QString(),
                    /*startedByTopLevel=*/name);
    reg.fail(name, error);
}

QVariantMap makeDep(const QString& name, const QString& version = {})
{
    QVariantMap d;
    d.insert(QStringLiteral("name"), name);
    if (!version.isEmpty()) d.insert(QStringLiteral("version"), version);
    return d;
}

// Shape returned by package_manager.getInstalledPackages() — matches the
// InstalledPackage JSON in logos-package-manager. Only the fields
// mergeLocalOnlyInstalled reads are populated here.
QVariantMap makeInstalledPackage(const QString& name,
                                 const QString& version,
                                 const QString& rootHash,
                                 const QString& category = {},
                                 const QString& installType = QStringLiteral("user"))
{
    QVariantMap hashes;
    hashes.insert(QStringLiteral("root"), rootHash);

    QVariantMap pkg;
    pkg.insert(QStringLiteral("name"),        name);
    pkg.insert(QStringLiteral("version"),     version);
    pkg.insert(QStringLiteral("hashes"),      hashes);
    pkg.insert(QStringLiteral("installType"), installType);
    if (!category.isEmpty())
        pkg.insert(QStringLiteral("category"), category);
    return pkg;
}

QVariantMap makeCatalogRow(const QString& repo,
                           const QString& name,
                           const QString& version,
                           const QString& rootHash,
                           const QVariantList& deps = {})
{
    // Matches the real catalog index shape used by logos-modules-release
    // (see https://raw.githubusercontent.com/dlipicar/logos-modules-release/
    // refs/heads/main/logos-repo.json → indexUrl). Version lives INSIDE
    // manifest, rootHash sits at the top of the version entry. Earlier
    // tests put `version` at the top of the version entry which let
    // AppsModel's version-in-manifest read miss silently — the prod
    // bug we're now catching.
    QVariantMap manifest;
    manifest.insert(QStringLiteral("version"),      version);
    manifest.insert(QStringLiteral("dependencies"), deps);

    QVariantMap versionEntry;
    versionEntry.insert(QStringLiteral("rootHash"), rootHash);
    versionEntry.insert(QStringLiteral("manifest"), manifest);

    QVariantMap row;
    row.insert(QStringLiteral("name"),          name);
    row.insert(QStringLiteral("repositoryUrl"), repo);
    row.insert(QStringLiteral("versions"),
               QVariantList{versionEntry});
    return row;
}

// Read InstallStatus for a (name, repo) row through the public role API.
InstallStatus::Value statusOf(const AppsModel& model,
                                  const QString& name,
                                  const QString& repo)
{
    int statusRole = -1, nameRole = -1, repoRole = -1;
    const auto& roles = model.roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if      (it.value() == "installStatus")  statusRole = it.key();
        else if (it.value() == "name")           nameRole   = it.key();
        else if (it.value() == "repositoryUrl")  repoRole   = it.key();
    }
    Q_ASSERT(statusRole >= 0 && nameRole >= 0 && repoRole >= 0);

    const int n = model.rowCount();
    for (int i = 0; i < n; ++i) {
        const QModelIndex idx = model.index(i);
        if (model.data(idx, nameRole).toString() != name) continue;
        if (model.data(idx, repoRole).toString() != repo) continue;
        return static_cast<InstallStatus::Value>(
            model.data(idx, statusRole).toInt());
    }
    qFatal("statusOf: row not found for (%s, %s)",
           qPrintable(name), qPrintable(repo));
    return InstallStatus::NotInstalled;
}

} // namespace

class AppsModelTest : public QObject {
    Q_OBJECT

private slots:
    // ── Fresh state ─────────────────────────────────────────────────────
    void notInstalledByDefault()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui"),
        });
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::NotInstalled);
    }

    // ── Single repo, identity & version ─────────────────────────────────
    void singleRepoInstalled()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui"),
        });
        model.markInstalled("wallet_ui", "1.0", "H_ui");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);
    }

    void singleRepoUpgradeAvailable()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "2.0", "H_v2"),
        });
        model.markInstalled("wallet_ui", "1.0", "H_v1");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::UpgradeAvailable);
    }

    void singleRepoDowngradeAvailable()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_v1"),
        });
        model.markInstalled("wallet_ui", "2.0", "H_v2");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::DowngradeAvailable);
    }

    void singleRepoDifferentHash_topLevel()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_repo1"),
        });
        // Same version, different binary.
        model.markInstalled("wallet_ui", "1.0", "H_otherBuild");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::DifferentHash);
    }

    // ── Multi-repo: identical builds → both Installed ───────────────────
    void twoReposSameHash_bothInstalled()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_shared"),
            makeCatalogRow("repo2", "wallet_ui", "1.0", "H_shared"),
        });
        model.markInstalled("wallet_ui", "1.0", "H_shared");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_ui", "repo2"),
                 InstallStatus::Installed);
    }

    // ── Multi-repo: different top-level hash ────────────────────────────
    void twoReposDifferentTopLevelHash()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H1"),
            makeCatalogRow("repo2", "wallet_ui", "1.0", "H2"),
        });
        model.markInstalled("wallet_ui", "1.0", "H1");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_ui", "repo2"),
                 InstallStatus::DifferentHash);
    }

    // ── Multi-repo: same top-level, different dep hash ──────────────────
    // The reported bug: wallet_ui shared between repos, wallet_modules
    // differs. Installing from repo1 must mark repo2's tile Reinstall.
    void twoReposDifferentDepHash()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui_shared",
                           { makeDep("wallet_modules") }),
            makeCatalogRow("repo2", "wallet_ui", "1.0", "H_ui_shared",
                           { makeDep("wallet_modules") }),
            makeCatalogRow("repo1", "wallet_modules", "1.0", "H_mods_repo1"),
            makeCatalogRow("repo2", "wallet_modules", "1.0", "H_mods_repo2"),
        });
        model.markInstalled("wallet_ui",      "1.0", "H_ui_shared");
        model.markInstalled("wallet_modules", "1.0", "H_mods_repo1");

        QCOMPARE(statusOf(model, "wallet_ui",      "repo1"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_ui",      "repo2"),
                 InstallStatus::DifferentHash);
        QCOMPARE(statusOf(model, "wallet_modules", "repo1"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_modules", "repo2"),
                 InstallStatus::DifferentHash);
    }

    // ── Partial install (deps missing) ──────────────────────────────────
    void missingDepsForcesNotInstalled()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui",
                           { makeDep("wallet_modules") }),
            makeCatalogRow("repo2", "wallet_ui", "1.0", "H_ui",
                           { makeDep("wallet_modules") }),
            makeCatalogRow("repo1", "wallet_modules", "1.0", "H_mods_repo1"),
            makeCatalogRow("repo2", "wallet_modules", "1.0", "H_mods_repo2"),
        });
        model.markInstalled("wallet_ui", "1.0", "H_ui");
        model.setMissingDeps("wallet_ui", { "wallet_modules" });

        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::NotInstalled);
        QCOMPARE(statusOf(model, "wallet_ui", "repo2"),
                 InstallStatus::NotInstalled);
    }

    // ── markInstalled cascades to dependents ────────────────────────────
    void markInstalledCascadesToDependents()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui",
                           { makeDep("wallet_modules") }),
            makeCatalogRow("repo2", "wallet_ui", "1.0", "H_ui",
                           { makeDep("wallet_modules") }),
            makeCatalogRow("repo1", "wallet_modules", "1.0", "H_mods_repo1"),
            makeCatalogRow("repo2", "wallet_modules", "1.0", "H_mods_repo2"),
        });
        model.markInstalled("wallet_ui",      "1.0", "H_ui");
        model.markInstalled("wallet_modules", "1.0", "H_mods_repo1");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_ui", "repo2"),
                 InstallStatus::DifferentHash);

        // Swap wallet_modules to repo2's hash.
        model.markInstalled("wallet_modules", "1.0", "H_mods_repo2");

        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::DifferentHash);
        QCOMPARE(statusOf(model, "wallet_ui", "repo2"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_modules", "repo1"),
                 InstallStatus::DifferentHash);
        QCOMPARE(statusOf(model, "wallet_modules", "repo2"),
                 InstallStatus::Installed);
    }

    // ── Uninstall ───────────────────────────────────────────────────────
    void uninstallReturnsToNotInstalled()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui"),
            makeCatalogRow("repo2", "wallet_ui", "1.0", "H_ui"),
        });
        model.markInstalled("wallet_ui", "1.0", "H_ui");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);

        model.markInstalled("wallet_ui", "", "");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::NotInstalled);
        QCOMPARE(statusOf(model, "wallet_ui", "repo2"),
                 InstallStatus::NotInstalled);
    }

    // ── Resolver-fallback: dep only in one repo ─────────────────────────
    void resolverFallbackDepNotInThisRepo()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui",
                           { makeDep("wallet_modules") }),
            makeCatalogRow("repo2", "wallet_ui", "1.0", "H_ui",
                           { makeDep("wallet_modules") }),
            makeCatalogRow("repo1", "wallet_modules", "1.0", "H_mods"),
        });
        model.markInstalled("wallet_ui",      "1.0", "H_ui");
        model.markInstalled("wallet_modules", "1.0", "H_mods");

        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);
        // repo2 doesn't publish wallet_modules — can't compare hashes; top-
        // level matches, so we don't downgrade.
        QCOMPARE(statusOf(model, "wallet_ui", "repo2"),
                 InstallStatus::Installed);
    }

    // ── Regression: version lives in manifest.version, not at the top ─
    // Previously AppsModel read versions[i].version, which is empty for the
    // real catalog shape (the index puts version inside manifest). That
    // made latestVersion = "" → recomputeInstallStatus's best-effort branch
    // returned Installed for every row regardless of state, skipping
    // version/hash/dep checks entirely. This test constructs a row by hand
    // (no helper) with version ONLY in manifest, then asserts the model
    // picks it up — older installedVersion ⇒ UpgradeAvailable, only
    // reachable when latestVersion is non-empty.
    void versionReadFromManifestNotTopLevel()
    {
        QVariantMap manifest;
        manifest.insert(QStringLiteral("version"), "1.0.0");
        manifest.insert(QStringLiteral("dependencies"), QVariantList{});

        QVariantMap versionEntry;
        // Deliberately omit top-level "version" — that's how the real index
        // looks. rootHash sits at the top of the version entry.
        versionEntry.insert(QStringLiteral("rootHash"), "H_v1");
        versionEntry.insert(QStringLiteral("manifest"), manifest);

        QVariantMap row;
        row.insert(QStringLiteral("name"),          "wallet_ui");
        row.insert(QStringLiteral("repositoryUrl"), "repo1");
        row.insert(QStringLiteral("versions"), QVariantList{versionEntry});

        AppsModel model;
        model.replaceCatalog({ row });

        // Installed at older version. With the bug, latestVersion was ""
        // so the compute returned Installed (best-effort). With the fix,
        // latestVersion="1.0.0", installedVersion="0.9.0" → UpgradeAvailable.
        model.markInstalled("wallet_ui", "0.9.0", "H_v0");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::UpgradeAvailable);

        // Same version, different hash → DifferentHash. Same story —
        // unreachable with the bug because the function bailed before
        // the hash check.
        model.markInstalled("wallet_ui", "1.0.0", "H_otherBuild");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::DifferentHash);
    }

    // ── End-to-end: actual logos-modules-release shape ─────────────────
    // Pins the exact (repo, package) layout the field hit — two repos
    // each publishing wallet_ui (depends on wallet_module) at the same
    // version but DIFFERENT root hashes. Installing repo1's binaries
    // must leave repo2's tile in DifferentHash. The row data here mirrors
    // (key-for-key) the JSON shape served by
    // https://raw.githubusercontent.com/dlipicar/logos-modules-release/
    // refs/heads/main/logos-repo.json → indexUrl, so any future schema
    // drift surfaces here first.
    void realCatalogShape_walletAcrossTwoRepos()
    {
        auto makeRealShape = [](const QString& repo,
                                const QString& name,
                                const QString& version,
                                const QString& rootHash,
                                const QVariantList& deps) {
            QVariantMap manifest;
            manifest.insert(QStringLiteral("version"),      version);
            manifest.insert(QStringLiteral("dependencies"), deps);
            // The real index also nests hashes here under
            // manifest.hashes.root — same value as the top-level rootHash.
            QVariantMap hashes;
            hashes.insert(QStringLiteral("root"), rootHash);
            manifest.insert(QStringLiteral("hashes"), hashes);

            QVariantMap versionEntry;
            versionEntry.insert(QStringLiteral("rootHash"),    rootHash);
            versionEntry.insert(QStringLiteral("releasedAt"),  "2025-01-01");
            versionEntry.insert(QStringLiteral("size"),        QVariant::fromValue<qlonglong>(123456));
            versionEntry.insert(QStringLiteral("url"),         "https://example/pkg.lgx");
            versionEntry.insert(QStringLiteral("sha256"),      rootHash);
            versionEntry.insert(QStringLiteral("manifest"),    manifest);

            QVariantMap row;
            row.insert(QStringLiteral("name"),          name);
            row.insert(QStringLiteral("repositoryUrl"), repo);
            row.insert(QStringLiteral("description"),   "");
            row.insert(QStringLiteral("type"),          name.endsWith("_ui") ? "ui_qml" : "core");
            row.insert(QStringLiteral("category"),      "");
            row.insert(QStringLiteral("versions"),      QVariantList{ versionEntry });
            return row;
        };

        AppsModel model;
        model.replaceCatalog({
            // repo1's wallet_ui depends on wallet_module — strings, like
            // the real manifest uses (["wallet_module"]).
            makeRealShape("repo1", "wallet_ui", "1.0.0",
                          "831f345bd9c9bc1b204be74b2b4b9b7f0b306aa0663662f15e18cd2244a78854",
                          QVariantList{ QVariant("wallet_module") }),
            makeRealShape("repo1", "wallet_module", "1.0.1",
                          "cf2f3db583f3ba9e60893ce8898d2400e769583166df00b1d90e032351f7033f",
                          QVariantList{}),
            // repo2 publishes the same packages but at different hashes
            // (the multi-repo case the user reported).
            makeRealShape("repo2", "wallet_ui", "1.0.0",
                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                          QVariantList{ QVariant("wallet_module") }),
            makeRealShape("repo2", "wallet_module", "1.0.1",
                          "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
                          QVariantList{}),
        });

        // User installs the wallet from repo1.
        model.markInstalled("wallet_ui", "1.0.0",
            "831f345bd9c9bc1b204be74b2b4b9b7f0b306aa0663662f15e18cd2244a78854");
        model.markInstalled("wallet_module", "1.0.1",
            "cf2f3db583f3ba9e60893ce8898d2400e769583166df00b1d90e032351f7033f");

        QCOMPARE(statusOf(model, "wallet_ui",     "repo1"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_module", "repo1"),
                 InstallStatus::Installed);
        // The bug we just fixed: this row was returning Installed in
        // production because latestVersion read empty and the compute
        // bailed before checking hashes.
        QCOMPARE(statusOf(model, "wallet_ui",     "repo2"),
                 InstallStatus::DifferentHash);
        QCOMPARE(statusOf(model, "wallet_module", "repo2"),
                 InstallStatus::DifferentHash);
    }

    // ── missingDeps cleared → flips back to Installed ───────────────────
    void missingDepsClearedFlipsBackToInstalled()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui",
                           { makeDep("wallet_modules") }),
            makeCatalogRow("repo1", "wallet_modules", "1.0", "H_mods"),
        });
        model.markInstalled("wallet_ui", "1.0", "H_ui");
        model.setMissingDeps("wallet_ui", { "wallet_modules" });
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::NotInstalled);

        model.markInstalled("wallet_modules", "1.0", "H_mods");
        model.setMissingDeps("wallet_ui", {});
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);
    }

    // ── Regression: setResolverOverlay must pin (repo, name) ────────────
    // Bug: with two repos publishing the same name, the second open of the
    // Add Application dialog left the clicked repo's rows with action=""
    // because setResolverOverlay used single-arg rowOf(name) and always
    // landed on the first-inserted row. PackageRowDelegate defaults action
    // empty → "Installed" badge, so the dialog showed "Installed" tags on
    // BOTH dep rows for a fresh install in repo2.
    void resolverOverlayPinsByRepo()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui",     "1.0", "H_ui1",
                           { makeDep("wallet_module") }),
            makeCatalogRow("repo1", "wallet_module", "1.0", "H_mod1"),
            makeCatalogRow("repo2", "wallet_ui",     "1.0", "H_ui2",
                           { makeDep("wallet_module") }),
            makeCatalogRow("repo2", "wallet_module", "1.0", "H_mod2"),
        });

        // Simulate the SECOND open of the dialog: click on repo2's tile.
        // The resolver result targets (wallet_ui, repo2) and
        // (wallet_module, repo2).
        AppsModel::ResolverRow top;
        top.name          = "wallet_ui";
        top.repositoryUrl = "repo2";
        top.action        = "install";
        top.toVersion     = "1.0";
        top.isTopLevel    = true;
        AppsModel::ResolverRow dep;
        dep.name          = "wallet_module";
        dep.repositoryUrl = "repo2";
        dep.action        = "install";
        dep.toVersion     = "1.0";
        model.setResolverOverlay({top, dep});

        // The action role must land on the repo2 rows so the dialog reads
        // "Install", not the default "Installed".
        int actionRole = -1, nameRole = -1, repoRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "action")        actionRole = it.key();
            else if (it.value() == "name")          nameRole   = it.key();
            else if (it.value() == "repositoryUrl") repoRole   = it.key();
        }
        QVERIFY(actionRole >= 0 && nameRole >= 0 && repoRole >= 0);

        auto actionFor = [&](const QString& name, const QString& repo) {
            const int n = model.rowCount();
            for (int i = 0; i < n; ++i) {
                const QModelIndex idx = model.index(i);
                if (model.data(idx, nameRole).toString() != name) continue;
                if (model.data(idx, repoRole).toString() != repo) continue;
                return model.data(idx, actionRole).toString();
            }
            return QString();
        };

        QCOMPARE(actionFor("wallet_ui",     "repo2"), QStringLiteral("install"));
        QCOMPARE(actionFor("wallet_module", "repo2"), QStringLiteral("install"));
        // repo1 rows must stay clean — the dialog's filter pins to repo2,
        // so repo1's rows are off-screen; their action being empty is fine,
        // but they must NOT be carrying the overlay either.
        QCOMPARE(actionFor("wallet_ui",     "repo1"), QString());
        QCOMPARE(actionFor("wallet_module", "repo1"), QString());
    }

    // ── Regression: multi-repo setInstallType / setIconUrl ─────────────
    // installType and iconUrl come from the installed binary, not the
    // catalog row. Every (repo, name) tile must show them; previously
    // the setters used rowOf(name) and only updated the first match.
    void installTypeAndIconUrlPropagateAcrossRepos()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H"),
            makeCatalogRow("repo2", "wallet_ui", "1.0", "H"),
        });
        model.setInstallType("wallet_ui", "user");
        model.setIconUrl("wallet_ui", "file:///path/to/icon.svg");

        int typeRole = -1, iconRole = -1, nameRole = -1, repoRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "installType")   typeRole = it.key();
            else if (it.value() == "iconUrl")       iconRole = it.key();
            else if (it.value() == "name")          nameRole = it.key();
            else if (it.value() == "repositoryUrl") repoRole = it.key();
        }
        QVERIFY(typeRole >= 0 && iconRole >= 0 && nameRole >= 0 && repoRole >= 0);

        auto fieldFor = [&](int role, const QString& name, const QString& repo) {
            const int n = model.rowCount();
            for (int i = 0; i < n; ++i) {
                const QModelIndex idx = model.index(i);
                if (model.data(idx, nameRole).toString() != name) continue;
                if (model.data(idx, repoRole).toString() != repo) continue;
                return model.data(idx, role).toString();
            }
            return QString();
        };

        QCOMPARE(fieldFor(typeRole, "wallet_ui", "repo1"), QStringLiteral("user"));
        QCOMPARE(fieldFor(typeRole, "wallet_ui", "repo2"), QStringLiteral("user"));
        QCOMPARE(fieldFor(iconRole, "wallet_ui", "repo1"),
                 QStringLiteral("file:///path/to/icon.svg"));
        QCOMPARE(fieldFor(iconRole, "wallet_ui", "repo2"),
                 QStringLiteral("file:///path/to/icon.svg"));
    }

    // ── Regression: per-row install stage isolation on partial failure ─
    // When the install loop reports row A success then row B failure,
    // each row's installStage must reflect ONLY its own outcome — no
    // cross-row contamination, even though they belong to the same
    // top-level session.
    void installStageIsolatedAcrossRowsOnPartialFailure()
    {
        AppsModel model;
        InstallRegistry reg;
        model.setInstallRegistry(&reg);
        model.replaceCatalog({
            makeCatalogRow("repo1", "module_a", "1.0", "H_a"),
            makeCatalogRow("repo1", "module_b", "1.0", "H_b"),
        });

        // Simulate installResultsSequential's per-row stage transitions.
        stage(reg, "module_a", InstallStage::Installing);
        stage(reg, "module_a", InstallStage::Installed);
        stage(reg, "module_b", InstallStage::Installing);
        stageFailed(reg, "module_b", "package_manager returned no path");

        int stageRole = -1, errorRole = -1, nameRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "installStage")  stageRole = it.key();
            else if (it.value() == "installError")  errorRole = it.key();
            else if (it.value() == "name")          nameRole  = it.key();
        }
        QVERIFY(stageRole >= 0 && errorRole >= 0 && nameRole >= 0);

        auto rowFor = [&](const QString& name) {
            const int n = model.rowCount();
            for (int i = 0; i < n; ++i) {
                const QModelIndex idx = model.index(i);
                if (model.data(idx, nameRole).toString() != name) continue;
                return idx;
            }
            return QModelIndex();
        };

        QCOMPARE(static_cast<InstallStage::Value>(
                    model.data(rowFor("module_a"), stageRole).toInt()),
                 InstallStage::Installed);
        QCOMPARE(model.data(rowFor("module_a"), errorRole).toString(),
                 QString());
        QCOMPARE(static_cast<InstallStage::Value>(
                    model.data(rowFor("module_b"), stageRole).toInt()),
                 InstallStage::Failed);
        QCOMPARE(model.data(rowFor("module_b"), errorRole).toString(),
                 QStringLiteral("package_manager returned no path"));
    }

    // ── Regression: coordinator clears sticky installStage on resolver
    // overlay apply ────────────────────────────────────────────────────
    //
    // Without this, a row left at InstallStage::Installed from a previous
    // install session keeps reading "Installed" in PackageRowDelegate's
    // stage switch on the next dialog open, masking a fresh action like
    // Upgrade or Reinstall.
    void resolverOverlayClearsStickyInstallStage()
    {
        AppsModel model;
        InstallRegistry reg;
        model.setInstallRegistry(&reg);
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_module", "1.0", "H_mod"),
        });
        stage(reg, "wallet_module", InstallStage::Installed);

        int stageRole = -1, nameRole = -1, repoRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "installStage")  stageRole = it.key();
            else if (it.value() == "name")          nameRole  = it.key();
            else if (it.value() == "repositoryUrl") repoRole  = it.key();
        }
        QVERIFY(stageRole >= 0 && nameRole >= 0 && repoRole >= 0);

        auto stageFor = [&](const QString& name, const QString& repo) {
            const int n = model.rowCount();
            for (int i = 0; i < n; ++i) {
                const QModelIndex idx = model.index(i);
                if (model.data(idx, nameRole).toString() != name) continue;
                if (model.data(idx, repoRole).toString() != repo) continue;
                return static_cast<InstallStage::Value>(
                    model.data(idx, stageRole).toInt());
            }
            return InstallStage::None;
        };

        QCOMPARE(stageFor("wallet_module", "repo1"), InstallStage::Installed);

        AppsModel::ResolverRow rr;
        rr.name          = "wallet_module";
        rr.repositoryUrl = "repo1";
        rr.action        = "reinstall";
        rr.toVersion     = "1.0";
        model.setResolverOverlay({rr});
        reg.clear("wallet_module");

        QCOMPARE(stageFor("wallet_module", "repo1"), InstallStage::None);
    }

    // ── Regression: dep-walk needs the DEP'S installedHash populated.
    // Reproduces the runtime bug where wallet_module (a core module, not a
    // UI plugin) never had its installedHash set on the AppsModel row, so
    // wallet_ui's dep walk skipped the per-dep DifferentHash check. The
    // fix in populateAppsModel iterates the full installed set, not just
    // the UI-plugin subset. This test stays at the model layer by simply
    // omitting the markInstalled call for the dep — same observable
    // effect.
    void dependentTileStaysInstalledWhenDepNotMarked()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0.0", "H_ui_same",
                           { makeDep("wallet_module") }),
            makeCatalogRow("repo1", "wallet_module", "1.0.1", "H_mod_a"),
            makeCatalogRow("repo2", "wallet_ui", "1.0.0", "H_ui_same",
                           { makeDep("wallet_module") }),
            makeCatalogRow("repo2", "wallet_module", "1.0.1", "H_mod_b"),
        });
        // Mark wallet_ui installed but NOT wallet_module — the prod bug
        // shape (PackageCoordinator's UI-only replay loop).
        model.markInstalled("wallet_ui", "1.0.0", "H_ui_same");

        // With dep installedHash missing, both wallet_ui rows incorrectly
        // resolve to Installed — captures the buggy behaviour.
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_ui", "repo2"),
                 InstallStatus::Installed);

        // After the full replay (now done by populateAppsModel), the dep
        // walk has data and repo2 correctly demotes to DifferentHash.
        model.markInstalled("wallet_module", "1.0.1", "H_mod_a");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_ui", "repo2"),
                 InstallStatus::DifferentHash);
    }

    // ── Mirrors EXACT runtime data from the user's restart-loses-Reinstall
    // report: both repos publish wallet_ui at the SAME hash, but
    // wallet_module differs. Tile state must be Installed on both wallet_ui
    // rows AND DifferentHash on dlipicar's wallet_module row AND
    // DifferentHash on dlipicar's wallet_ui row (via dep walk against the
    // mismatched wallet_module).
    void userRuntimeData_walletMultiRepoDepWalk()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("logos-co", "wallet_ui", "1.0.0",
                           "831f345bd9c9...",
                           { makeDep("wallet_module") }),
            makeCatalogRow("logos-co", "wallet_module", "1.0.1",
                           "73a30234d0f7..."),
            makeCatalogRow("dlipicar", "wallet_ui", "1.0.0",
                           "831f345bd9c9...",   // SAME as logos-co
                           { makeDep("wallet_module") }),
            makeCatalogRow("dlipicar", "wallet_module", "1.0.1",
                           "cf2f3db583f3..."),  // DIFFERENT from logos-co
        });
        // Installed binaries (per user's runtime log).
        model.markInstalled("wallet_ui",     "1.0.0", "831f345bd9c9...");
        model.markInstalled("wallet_module", "1.0.1", "73a30234d0f7...");

        // logos-co matches on-disk binary for both → Installed.
        QCOMPARE(statusOf(model, "wallet_ui",     "logos-co"),
                 InstallStatus::Installed);
        QCOMPARE(statusOf(model, "wallet_module", "logos-co"),
                 InstallStatus::Installed);
        // dlipicar's wallet_module differs from on-disk → DifferentHash.
        QCOMPARE(statusOf(model, "wallet_module", "dlipicar"),
                 InstallStatus::DifferentHash);
        // dlipicar's wallet_ui: same top-level hash as on-disk, BUT its
        // declared dep wallet_module differs against dlipicar's catalog →
        // dep walk must demote it to DifferentHash.
        QCOMPARE(statusOf(model, "wallet_ui",     "dlipicar"),
                 InstallStatus::DifferentHash);
    }

    // ── Idempotence + role notification contracts ──────────────────────
    // These lock the "calling a setter with the same value is a silent
    // no-op" contract that QML bindings depend on — a redundant
    // dataChanged emit can re-trigger expensive QML re-renders, and a
    // missing emit hides real changes from the view.

    void markInstalled_idempotent_no_op_when_unchanged()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H"),
        });
        model.markInstalled("wallet_ui", "1.0", "H");

        // Spy on dataChanged. A second mark with identical (version, hash)
        // must not emit — the row already holds those values.
        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        model.markInstalled("wallet_ui", "1.0", "H");
        QCOMPARE(spy.count(), 0);
    }

    void replaceCatalog_removes_rows_not_in_incoming()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui",     "1.0", "H1"),
            makeCatalogRow("repo1", "wallet_module", "1.0", "H2"),
        });
        QCOMPARE(model.rowCount(), 2);
        // wallet_module disappears from the catalog.
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H1"),
        });
        QCOMPARE(model.rowCount(), 1);
    }

    void replaceCatalog_preserves_installState_on_existing_row_update()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H"),
        });
        model.markInstalled("wallet_ui", "1.0", "H");
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);

        // Re-issue the catalog — same shape, same row identity. The
        // installed state must survive the refresh; we shouldn't see a
        // tile blink to NotInstalled mid-refresh.
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H"),
        });
        QCOMPARE(statusOf(model, "wallet_ui", "repo1"),
                 InstallStatus::Installed);
    }

    void setMissingDeps_unknownName_is_silent_noop()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H"),
        });
        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        // No row with this name — must not crash, must not emit.
        model.setMissingDeps("does_not_exist", { "foo" });
        QCOMPARE(spy.count(), 0);
    }

    void clearResolverOverlay_resets_action_and_isTopLevel()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H"),
        });
        AppsModel::ResolverRow rr;
        rr.name = "wallet_ui"; rr.repositoryUrl = "repo1";
        rr.action = "install"; rr.toVersion = "1.0"; rr.isTopLevel = true;
        model.setResolverOverlay({rr});

        int actionRole = -1, isTopRole = -1, nameRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "action")     actionRole = it.key();
            else if (it.value() == "isTopLevel") isTopRole  = it.key();
            else if (it.value() == "name")       nameRole   = it.key();
        }
        const QModelIndex idx = [&]{
            for (int i = 0; i < model.rowCount(); ++i) {
                const QModelIndex mi = model.index(i);
                if (model.data(mi, nameRole).toString() == "wallet_ui") return mi;
            }
            return QModelIndex{};
        }();
        QVERIFY(idx.isValid());
        QCOMPARE(model.data(idx, actionRole).toString(), QStringLiteral("install"));
        QCOMPARE(model.data(idx, isTopRole).toBool(),    true);

        model.clearResolverOverlay();
        QCOMPARE(model.data(idx, actionRole).toString(), QString());
        QCOMPARE(model.data(idx, isTopRole).toBool(),    false);
    }

    void recomputeInstallStatus_emptyCatalogVersion_falls_back_to_Installed()
    {
        // Catalog row exists but has no usable version data (older publishers,
        // partial entries). Once installed, recomputeInstallStatus has nothing
        // to compare against — best-effort Installed, NOT NotInstalled.
        AppsModel model;
        QVariantMap row;
        row.insert("name",          "ghost_app");
        row.insert("repositoryUrl", "repo1");
        row.insert("versions",      QVariantList{});  // empty
        model.replaceCatalog({ row });
        model.markInstalled("ghost_app", "1.0", "H");
        QCOMPARE(statusOf(model, "ghost_app", "repo1"),
                 InstallStatus::Installed);
    }

    void emptyCatalog_zero_rows_no_crash()
    {
        AppsModel model;
        InstallRegistry reg;
        model.setInstallRegistry(&reg);
        model.replaceCatalog({});
        QCOMPARE(model.rowCount(), 0);
        // Mutators against empty model are no-ops — no row for AppsModel to
        // mutate, and stage() through the registry has no matching row so
        // the InstallStageRole read still yields None.
        model.markInstalled("anything", "1.0", "H");
        model.setMissingDeps("anything", { "x" });
        stage(reg, "anything", InstallStage::Installed);
        QCOMPARE(model.rowCount(), 0);
    }

    // ── Role-name contract ────────────────────────────────────────────
    // QML reads roles by NAME (e.g. `model.installStatus`, `appRow.action`),
    // so renaming any of these silently breaks every binding. Locks the
    // exact string each role serialises to.
    void roleNames_stable()
    {
        AppsModel model;
        const auto roles = model.roleNames();
        QHash<QByteArray, bool> seen;
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) seen.insert(it.value(), true);
        const QList<QByteArray> required{
            "name", "repositoryUrl", "displayName", "description", "category",
            "type", "color", "iconUrl", "versions", "dependencies", "installedVersion",
            "latestVersion", "hasUpdate", "isInstalled", "missingDeps",
            "installStatus", "installType", "action", "toVersion",
            "isTopLevel", "resolverError", "installStage", "installError",
        };
        for (const QByteArray& r : required)
            QVERIFY2(seen.contains(r), qPrintable("missing role: " + r));
    }

    // ── Schema: every produced row's dependencies field is a normalised
    // list of {name, version} maps regardless of input shape (string vs
    // object). The dialog filter + dep walk both read `dep.name`; a row
    // dropped silently here cascades into every multi-repo Reinstall
    // detection downstream.
    void dependencies_normalise_string_and_object_inputs()
    {
        AppsModel model;
        QVariantMap manifestStr;
        manifestStr.insert("version",      "1.0");
        manifestStr.insert("dependencies", QVariantList{ QVariant("wallet_module") });
        QVariantMap manifestObj;
        manifestObj.insert("version",      "1.0");
        manifestObj.insert("dependencies", QVariantList{
            QVariantMap{ {"name", "wallet_module"}, {"version", ">=1.0"} },
        });
        QVariantMap rowStr;
        rowStr.insert("name", "uses_string_dep");
        rowStr.insert("repositoryUrl", "r");
        rowStr.insert("versions", QVariantList{ QVariantMap{
            {"rootHash", "H"}, {"manifest", manifestStr}
        }});
        QVariantMap rowObj;
        rowObj.insert("name", "uses_object_dep");
        rowObj.insert("repositoryUrl", "r");
        rowObj.insert("versions", QVariantList{ QVariantMap{
            {"rootHash", "H"}, {"manifest", manifestObj}
        }});
        model.replaceCatalog({ rowStr, rowObj });

        int depsRole = -1, nameRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "dependencies") depsRole = it.key();
            else if (it.value() == "name")         nameRole = it.key();
        }
        QVERIFY(depsRole >= 0 && nameRole >= 0);

        for (int i = 0; i < model.rowCount(); ++i) {
            const QModelIndex idx = model.index(i);
            const QVariantList deps = model.data(idx, depsRole).toList();
            QCOMPARE(deps.size(), 1);
            QCOMPARE(deps.first().toMap().value("name").toString(),
                     QStringLiteral("wallet_module"));
        }
    }

    // ── mergeLocalOnlyInstalled + replaceCatalog local-preservation ────
    // Locks the "installed-but-uncatalogued" behaviour: names in the
    // installed-set that the catalog doesn't publish get a synthetic row
    // with empty repositoryUrl (rendered under a "Local" section). Names
    // the catalog does publish are the catalog's job — mergeLocalOnly
    // must not double-insert.

    void mergeLocalOnly_adds_row_for_uncatalogued_installed()
    {
        AppsModel model;
        model.replaceCatalog({});   // no repos configured
        model.mergeLocalOnlyInstalled({
            makeInstalledPackage("side_loaded_module", "1.2.3", "H_side", "utilities"),
        });
        QCOMPARE(model.rowCount(), 1);

        int nameRole = -1, repoRole = -1, catRole = -1, verRole = -1,
            typeRole = -1, statusRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "name")             nameRole   = it.key();
            else if (it.value() == "repositoryUrl")    repoRole   = it.key();
            else if (it.value() == "category")         catRole    = it.key();
            else if (it.value() == "installedVersion") verRole    = it.key();
            else if (it.value() == "installType")      typeRole   = it.key();
            else if (it.value() == "installStatus")    statusRole = it.key();
        }
        const QModelIndex idx = model.index(0);
        QCOMPARE(model.data(idx, nameRole).toString(),
                 QStringLiteral("side_loaded_module"));
        // Repo empty = the "Local" bucket marker.
        QCOMPARE(model.data(idx, repoRole).toString(), QString());
        // Module's own category survives — Local is a repo slot, not a
        // category override.
        QCOMPARE(model.data(idx, catRole).toString(),  QStringLiteral("utilities"));
        QCOMPARE(model.data(idx, verRole).toString(),  QStringLiteral("1.2.3"));
        QCOMPARE(model.data(idx, typeRole).toString(), QStringLiteral("user"));
        // No catalog version to compare against → Installed best-effort.
        QCOMPARE(static_cast<InstallStatus::Value>(model.data(idx, statusRole).toInt()),
                 InstallStatus::Installed);
    }

    void mergeLocalOnly_skips_names_covered_by_catalog()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui",  "1.0", "H_repo"),
        });
        model.mergeLocalOnlyInstalled({
            // Catalog already has wallet_ui → NO synthetic row.
            makeInstalledPackage("wallet_ui",       "1.0", "H_repo",  "wallet"),
            // No catalog row for orphan_mod → gets a Local row.
            makeInstalledPackage("orphan_mod", "1.0", "H_orphan", "misc"),
        });
        // 1 catalog row + 1 local row (wallet_ui not duplicated).
        QCOMPARE(model.rowCount(), 2);

        int nameRole = -1, repoRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "name")          nameRole = it.key();
            else if (it.value() == "repositoryUrl") repoRole = it.key();
        }
        // wallet_ui stays on its catalog row (repo1). orphan_mod is Local.
        bool sawWalletOnRepo = false, sawOrphanOnLocal = false;
        for (int i = 0; i < model.rowCount(); ++i) {
            const QModelIndex idx = model.index(i);
            const QString n = model.data(idx, nameRole).toString();
            const QString r = model.data(idx, repoRole).toString();
            if (n == "wallet_ui"  && r == "repo1") sawWalletOnRepo = true;
            if (n == "orphan_mod" && r == "")      sawOrphanOnLocal = true;
        }
        QVERIFY(sawWalletOnRepo);
        QVERIFY(sawOrphanOnLocal);
    }

    void mergeLocalOnly_is_idempotent()
    {
        AppsModel model;
        model.mergeLocalOnlyInstalled({
            makeInstalledPackage("orphan_mod", "1.0", "H", "misc"),
        });
        QCOMPARE(model.rowCount(), 1);
        // Second call with the same input must not duplicate.
        model.mergeLocalOnlyInstalled({
            makeInstalledPackage("orphan_mod", "1.0", "H", "misc"),
        });
        QCOMPARE(model.rowCount(), 1);
    }

    // ── Regression: uninstalling a local-only module removes its row ────
    //
    // Bug: mergeLocalOnlyInstalled is called by PackageCoordinator after
    // every install/uninstall/reload with the FRESH installed-packages
    // list. When a user uninstalls a local-only module via PMUI:
    //   - PMUI removes it from its own state (row disappears from PMUI).
    //   - PackageCoordinator's uiPluginUninstalled handler fires refresh.
    //   - refreshDependencyInfo repopulates m_installedPackagesCache with
    //     the fresh list (module gone).
    //   - AppsModel::mergeLocalOnlyInstalled(fresh_cache) runs.
    // The row must vanish. Before the fix, mergeLocalOnlyInstalled was
    // add-only — the previously-inserted local row survived because the
    // merge iterates the fresh list and only adds; there was no removal
    // pass for names that dropped out. Reload runs the same code path so
    // nothing recovered until an app restart.
    void mergeLocalOnly_removes_row_when_installed_set_drops_the_name()
    {
        AppsModel model;
        model.replaceCatalog({});
        model.mergeLocalOnlyInstalled({
            makeInstalledPackage("orphan_mod", "1.0", "H", "misc"),
        });
        QCOMPARE(model.rowCount(), 1);

        // User uninstalls orphan_mod → next merge sees an empty user set.
        // The Local row for it must disappear.
        model.mergeLocalOnlyInstalled({});
        QCOMPARE(model.rowCount(), 0);
    }

    // Same reconciliation with a catalog present: catalog rows are the
    // catalog's job (replaceCatalog owns them), so they must NOT be
    // affected by the local-only prune. Only rows with empty
    // repositoryUrl whose name is no longer in the fresh installed set
    // are dropped.
    void mergeLocalOnly_removes_only_local_rows_leaves_catalog_intact()
    {
        AppsModel model;
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui"),
        });
        model.mergeLocalOnlyInstalled({
            makeInstalledPackage("orphan_mod", "1.0", "H_orphan", "misc"),
        });
        QCOMPARE(model.rowCount(), 2);   // catalog + local

        // Uninstall orphan_mod. wallet_ui is not user-installed either
        // (empty fresh set) — its catalog row must still survive because
        // catalog rows are managed by replaceCatalog, not by the local
        // merge.
        model.mergeLocalOnlyInstalled({});
        QCOMPARE(model.rowCount(), 1);

        int nameRole = -1, repoRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "name")          nameRole = it.key();
            else if (it.value() == "repositoryUrl") repoRole = it.key();
        }
        const QModelIndex idx = model.index(0);
        QCOMPARE(model.data(idx, nameRole).toString(), QStringLiteral("wallet_ui"));
        QCOMPARE(model.data(idx, repoRole).toString(), QStringLiteral("repo1"));
    }

    // Embedded packages ship inside the app bundle, not under Application
    // Support. They're already surfaced through the built-in module list —
    // synthesising a Local row for them would double-list. Gate: only
    // installType == "user" packages become Local rows.
    void mergeLocalOnly_skips_embedded_installType()
    {
        AppsModel model;
        model.mergeLocalOnlyInstalled({
            makeInstalledPackage("embedded_mod", "1.0", "H_emb", "misc",
                                 QStringLiteral("embedded")),
            makeInstalledPackage("user_mod",     "2.0", "H_usr", "misc",
                                 QStringLiteral("user")),
        });
        QCOMPARE(model.rowCount(), 1);

        int nameRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if (it.value() == "name") { nameRole = it.key(); break; }
        }
        QCOMPARE(model.data(model.index(0), nameRole).toString(),
                 QStringLiteral("user_mod"));
    }

    void replaceCatalog_preserves_local_row_when_catalog_still_lacks_it()
    {
        AppsModel model;
        model.replaceCatalog({});
        model.mergeLocalOnlyInstalled({
            makeInstalledPackage("orphan_mod", "1.0", "H", "misc"),
        });
        QCOMPARE(model.rowCount(), 1);

        // A subsequent catalog refresh — different repos come and go — must
        // not sweep away the local row that no repo has adopted yet.
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui"),
        });
        QCOMPARE(model.rowCount(), 2);   // catalog row + local row

        int nameRole = -1, repoRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "name")          nameRole = it.key();
            else if (it.value() == "repositoryUrl") repoRole = it.key();
        }
        bool sawOrphanLocal = false;
        for (int i = 0; i < model.rowCount(); ++i) {
            const QModelIndex idx = model.index(i);
            if (model.data(idx, nameRole).toString() == "orphan_mod"
                && model.data(idx, repoRole).toString().isEmpty()) {
                sawOrphanLocal = true;
                break;
            }
        }
        QVERIFY(sawOrphanLocal);
    }

    void replaceCatalog_drops_local_row_when_catalog_adopts_the_name()
    {
        AppsModel model;
        model.replaceCatalog({});
        model.mergeLocalOnlyInstalled({
            makeInstalledPackage("wallet_ui", "1.0", "H_installed", "wallet"),
        });
        QCOMPARE(model.rowCount(), 1);

        // A repo is added and now publishes wallet_ui — the local row must
        // go away, replaced by the catalog row.
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_installed"),
        });
        QCOMPARE(model.rowCount(), 1);

        int nameRole = -1, repoRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if      (it.value() == "name")          nameRole = it.key();
            else if (it.value() == "repositoryUrl") repoRole = it.key();
        }
        const QModelIndex idx = model.index(0);
        QCOMPARE(model.data(idx, nameRole).toString(), QStringLiteral("wallet_ui"));
        // Now under repo1, not the Local bucket.
        QCOMPARE(model.data(idx, repoRole).toString(), QStringLiteral("repo1"));
    }

    void replaceCatalog_preserves_color_field()
    {
        AppsModel model;
        QVariantMap row;
        row.insert("name", "storage_ui");
        row.insert("repositoryUrl", "https://example/repo.json");
        row.insert("color", "#4a90d9");
        row.insert("versions", QVariantList{ QVariantMap{
            {"manifest", QVariantMap{ {"version", "1.0.0"} }}
        }});
        model.replaceCatalog({ row });

        int colorRole = -1;
        const auto& roles = model.roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if (it.value() == "color") { colorRole = it.key(); break; }
        }
        QVERIFY(colorRole >= 0);
        QCOMPARE(model.data(model.index(0), colorRole).toString(),
                 QStringLiteral("#4a90d9"));
    }
};

QTEST_GUILESS_MAIN(AppsModelTest)
#include "apps_model_test.moc"
