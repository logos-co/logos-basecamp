// srcdeps: AppsModel.cpp InstallEnums.cpp InstallRegistry.cpp
//
// Unit tests for live download progress: InstallRegistry's byte bookkeeping
// and the two AppsModel roles that surface it to QML.
//
// Progress originates in package_downloader (its `downloadProgress` event,
// forwarded by PackageCoordinator) and lands here. These tests cover the
// receiving half — no IPC, no PackageCoordinator, no display. Run locally:
//
//   nix build .#unit-tests -L

#include "AppsModel.h"
#include "InstallEnums.h"
#include "InstallRegistry.h"

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>

namespace {

QVariantMap makeCatalogRow(const QString& repo,
                           const QString& name,
                           const QString& version,
                           const QString& rootHash)
{
    QVariantMap manifest;
    manifest.insert(QStringLiteral("version"),      version);
    manifest.insert(QStringLiteral("dependencies"), QVariantList{});

    QVariantMap versionEntry;
    versionEntry.insert(QStringLiteral("rootHash"), rootHash);
    versionEntry.insert(QStringLiteral("manifest"), manifest);

    QVariantMap row;
    row.insert(QStringLiteral("name"),          name);
    row.insert(QStringLiteral("repositoryUrl"), repo);
    row.insert(QStringLiteral("versions"),      QVariantList{versionEntry});
    return row;
}

// Put `name` into the registry and move it to Downloading — the only stage
// that accepts byte progress.
void beginDownloading(InstallRegistry& reg, const QString& name)
{
    reg.begin(name, /*targetVersion=*/QString(), /*targetHash=*/QString(),
              /*startedByTopLevel=*/name);
    reg.setStage(name, InstallStage::Downloading);
}

// Read a role off the row for `name` by walking the model.
QVariant roleOf(AppsModel& model, const QString& name, int role)
{
    for (int i = 0; i < model.rowCount(); ++i) {
        const QModelIndex mi = model.index(i);
        if (model.data(mi, AppsModel::NameRole).toString() == name)
            return model.data(mi, role);
    }
    return {};
}

} // namespace

class DownloadProgressTest : public QObject {
    Q_OBJECT

private slots:
    // ── InstallRegistry bookkeeping ─────────────────────────────────────

    void progressIsZeroBeforeAnySampleArrives()
    {
        InstallRegistry reg;
        beginDownloading(reg, "wallet_ui");
        QCOMPARE(reg.downloadReceived("wallet_ui"), quint64(0));
        QCOMPARE(reg.downloadTotal("wallet_ui"), quint64(0));
    }

    void progressIsRecordedWhileDownloading()
    {
        InstallRegistry reg;
        beginDownloading(reg, "wallet_ui");
        reg.setDownloadProgress("wallet_ui", 1024, 4096);
        QCOMPARE(reg.downloadReceived("wallet_ui"), quint64(1024));
        QCOMPARE(reg.downloadTotal("wallet_ui"), quint64(4096));
    }

    // An unknown total means "the transport didn't say". Blanking a
    // denominator we already had would drop the bar back to indeterminate
    // mid-download.
    void unknownTotalKeepsThePreviousTotal()
    {
        InstallRegistry reg;
        beginDownloading(reg, "wallet_ui");
        reg.setDownloadProgress("wallet_ui", 1024, 4096);
        reg.setDownloadProgress("wallet_ui", 2048, 0);
        QCOMPARE(reg.downloadReceived("wallet_ui"), quint64(2048));
        QCOMPARE(reg.downloadTotal("wallet_ui"), quint64(4096));
    }

    // Progress for something the registry isn't tracking must not create
    // an entry — that would put a phantom row into activeNames().
    void progressForAnUntrackedPackageIsIgnored()
    {
        InstallRegistry reg;
        reg.setDownloadProgress("never_started", 512, 1024);
        QCOMPARE(reg.downloadReceived("never_started"), quint64(0));
        QVERIFY(!reg.has("never_started"));
        QVERIFY(reg.activeNames().isEmpty());
    }

    // The event crosses a process boundary, so a sample can land just after
    // the stage advanced. Applying it would redraw a download bar on a row
    // that is already installing.
    void lateProgressAfterTheDownloadStageIsIgnored()
    {
        InstallRegistry reg;
        beginDownloading(reg, "wallet_ui");
        reg.setDownloadProgress("wallet_ui", 1024, 4096);
        reg.setStage("wallet_ui", InstallStage::Installing);

        reg.setDownloadProgress("wallet_ui", 4096, 4096);
        QCOMPARE(reg.downloadReceived("wallet_ui"), quint64(1024));
        QCOMPARE(reg.stage("wallet_ui"), int(InstallStage::Installing));
    }

    // Counters SURVIVE the stage change: they record a completed transfer,
    // and the app's tile sums them across the plan. Zeroing a package as it
    // moved on to installing made the tile's bar jump backwards each time
    // one finished. Whether a bar is drawn is gated on the stage in QML.
    void leavingTheDownloadStageKeepsTheCounters()
    {
        InstallRegistry reg;
        beginDownloading(reg, "wallet_ui");
        reg.setDownloadProgress("wallet_ui", 4096, 4096);
        QCOMPARE(reg.downloadReceived("wallet_ui"), quint64(4096));

        reg.setStage("wallet_ui", InstallStage::Installing);
        QCOMPARE(reg.downloadReceived("wallet_ui"), quint64(4096));
        QCOMPARE(reg.downloadTotal("wallet_ui"), quint64(4096));
    }

    void progressEmitsItsOwnSignalNotStageChanged()
    {
        InstallRegistry reg;
        beginDownloading(reg, "wallet_ui");

        QSignalSpy progressSpy(&reg, &InstallRegistry::downloadProgressChanged);
        QSignalSpy stageSpy(&reg, &InstallRegistry::stageChanged);

        reg.setDownloadProgress("wallet_ui", 1024, 4096);

        QCOMPARE(progressSpy.count(), 1);
        QCOMPARE(progressSpy.first().at(0).toString(), QStringLiteral("wallet_ui"));
        // Progress fires several times a second; if it also emitted
        // stageChanged, every consumer would refresh the whole row.
        QCOMPARE(stageSpy.count(), 0);
    }

    void clearedEntryReportsNoProgress()
    {
        InstallRegistry reg;
        beginDownloading(reg, "wallet_ui");
        reg.setDownloadProgress("wallet_ui", 1024, 4096);
        reg.clear("wallet_ui");
        QCOMPARE(reg.downloadReceived("wallet_ui"), quint64(0));
        QCOMPARE(reg.downloadTotal("wallet_ui"), quint64(0));
    }

    // ── Plan aggregation (tiles) ────────────────────────────────────────
    //
    // A tile is an APP. Clicking it installs a whole plan, so its bar has to
    // span every package in that plan — otherwise it sits idle while the
    // dependencies, usually the large artifacts, download.

    void planAggregatesAcrossEveryPackage()
    {
        InstallRegistry reg;
        reg.begin("wallet_ui", {}, {}, "wallet_ui");
        reg.beginPlan("wallet_ui", {
            {"wallet_ui",     "1.0", "H_ui", 1000},
            {"wallet_module", "1.0", "H_mod", 4000},
        });

        QCOMPARE(reg.planDownloadTotal("wallet_ui"), quint64(5000));
        QCOMPARE(reg.planDownloadReceived("wallet_ui"), quint64(0));

        reg.setDownloadProgress("wallet_module", 2000, 4000);
        reg.setDownloadProgress("wallet_ui", 500, 1000);
        QCOMPARE(reg.planDownloadReceived("wallet_ui"), quint64(2500));
    }

    // The denominator must be right before a single byte moves, so the tile
    // never shows a total that grows as packages appear.
    void planTotalIsSeededFromTheCatalogBeforeAnyBytes()
    {
        InstallRegistry reg;
        reg.beginPlan("wallet_ui", {
            {"wallet_ui", "1.0", "H_ui", 1000},
            {"dep_a",     "1.0", "H_a",  2000},
            {"dep_b",     "1.0", "H_b",  3000},
        });
        QCOMPARE(reg.planDownloadTotal("wallet_ui"), quint64(6000));
    }

    // A dependency's bytes move the app's aggregate, and the app is a
    // different model row — it has to be told, or the tile never repaints.
    void dependencyProgressNotifiesItsTopLevel()
    {
        InstallRegistry reg;
        reg.beginPlan("wallet_ui", {
            {"wallet_ui",     "1.0", "H_ui", 1000},
            {"wallet_module", "1.0", "H_mod", 4000},
        });

        QSignalSpy spy(&reg, &InstallRegistry::downloadProgressChanged);
        reg.setDownloadProgress("wallet_module", 2000, 4000);

        QStringList notified;
        for (const auto& args : spy) notified << args.at(0).toString();
        QVERIFY(notified.contains("wallet_module"));
        QVERIFY2(notified.contains("wallet_ui"),
                 "the app row must be invalidated when a dependency progresses");
    }

    // A dependency row in the confirm dialog is not a top-level, so the same
    // accessor must degrade to that package's own bytes.
    void planFallsBackToOwnBytesForANonTopLevelPackage()
    {
        InstallRegistry reg;
        reg.beginPlan("wallet_ui", {
            {"wallet_ui",     "1.0", "H_ui", 1000},
            {"wallet_module", "1.0", "H_mod", 4000},
        });
        reg.setDownloadProgress("wallet_module", 2000, 4000);

        QCOMPARE(reg.planDownloadReceived("wallet_module"), quint64(2000));
        QCOMPARE(reg.planDownloadTotal("wallet_module"), quint64(4000));
    }

    // package_downloader fetches a plan strictly one package at a time, so
    // exactly one entry may read as Downloading. Staging the whole plan as
    // Downloading claimed simultaneous transfers that never happen.
    void plannedPackagesStartQueuedNotDownloading()
    {
        InstallRegistry reg;
        reg.begin("wallet_ui", {}, {}, "wallet_ui");
        reg.beginPlan("wallet_ui", {{"wallet_ui", "1.0", "H_ui", 1000},
                                    {"wallet_module", "1.0", "H_mod", 4000}});

        QCOMPARE(reg.stage("wallet_module"), int(InstallStage::Queued));
    }

    // The queued package becomes the downloading one when its bytes start
    // arriving — that is what makes the sequential order visible.
    void firstBytesPromoteAQueuedPackageToDownloading()
    {
        InstallRegistry reg;
        reg.beginPlan("wallet_ui", {{"dep_a", "1.0", "H_a", 2000},
                                    {"dep_b", "1.0", "H_b", 3000}});
        QCOMPARE(reg.stage("dep_a"), int(InstallStage::Queued));

        QSignalSpy stageSpy(&reg, &InstallRegistry::stageChanged);
        reg.setDownloadProgress("dep_a", 500, 2000);

        QCOMPARE(reg.stage("dep_a"), int(InstallStage::Downloading));
        QCOMPARE(reg.downloadReceived("dep_a"), quint64(500));
        QVERIFY2(stageSpy.count() >= 1, "the promotion must be announced");
        // The one that has not started yet stays queued.
        QCOMPARE(reg.stage("dep_b"), int(InstallStage::Queued));
    }

    // Only one package downloads at a time, but the plan total must still
    // cover the queued ones or the denominator would grow as they start.
    void queuedPackagesStillCountTowardThePlanTotal()
    {
        InstallRegistry reg;
        reg.beginPlan("wallet_ui", {{"dep_a", "1.0", "H_a", 2000},
                                    {"dep_b", "1.0", "H_b", 3000}});
        reg.setDownloadProgress("dep_a", 2000, 2000);

        QCOMPARE(reg.planDownloadTotal("wallet_ui"), quint64(5000));
        QCOMPARE(reg.planDownloadReceived("wallet_ui"), quint64(2000));
    }

    // ── Concurrent installs sharing a dependency ────────────────────────

    void twoInstallsAggregateIndependently()
    {
        InstallRegistry reg;
        reg.beginPlan("app_a", {{"app_a", "1.0", "H_a", 1000},
                                {"dep_a", "1.0", "H_da", 2000}});
        reg.beginPlan("app_b", {{"app_b", "1.0", "H_b", 5000}});

        QCOMPARE(reg.planDownloadTotal("app_a"), quint64(3000));
        QCOMPARE(reg.planDownloadTotal("app_b"), quint64(5000));

        reg.setDownloadProgress("dep_a", 1000, 2000);
        QCOMPARE(reg.planDownloadReceived("app_a"), quint64(1000));
        QCOMPARE(reg.planDownloadReceived("app_b"), quint64(0));
    }

    // A package both apps need counts toward BOTH bars — attributing it to
    // whichever install registered first would leave the other undercounting
    // and stuck short of full.
    void sharedDependencyCountsTowardBothInstalls()
    {
        InstallRegistry reg;
        reg.beginPlan("app_a", {{"app_a", "1.0", "H_a", 1000},
                                {"shared", "1.0", "H_s", 4000}});
        reg.beginPlan("app_b", {{"app_b", "1.0", "H_b", 2000},
                                {"shared", "1.0", "H_s", 4000}});

        QCOMPARE(reg.planDownloadTotal("app_a"), quint64(5000));
        QCOMPARE(reg.planDownloadTotal("app_b"), quint64(6000));

        reg.setDownloadProgress("shared", 4000, 4000);
        QCOMPARE(reg.planDownloadReceived("app_a"), quint64(4000));
        QCOMPARE(reg.planDownloadReceived("app_b"), quint64(4000));
    }

    // Joining an in-flight install must not rewind the one already running.
    void joiningASharedDependencyDoesNotResetItsProgress()
    {
        InstallRegistry reg;
        reg.beginPlan("app_a", {{"shared", "1.0", "H_s", 4000}});
        reg.setDownloadProgress("shared", 3000, 4000);

        reg.beginPlan("app_b", {{"shared", "1.0", "H_s", 4000}});
        QCOMPARE(reg.downloadReceived("shared"), quint64(3000));
        QCOMPARE(reg.planDownloadReceived("app_b"), quint64(3000));
    }

    // Finishing one install must not evict a package the other is still
    // downloading.
    void clearingOneInstallKeepsASharedDependencyForTheOther()
    {
        InstallRegistry reg;
        reg.beginPlan("app_a", {{"app_a", "1.0", "H_a", 1000},
                                {"shared", "1.0", "H_s", 4000}});
        reg.beginPlan("app_b", {{"app_b", "1.0", "H_b", 2000},
                                {"shared", "1.0", "H_s", 4000}});

        reg.clearByTopLevel("app_a");

        QVERIFY2(reg.has("shared"), "app_b is still downloading it");
        QVERIFY(!reg.has("app_a"));
        QCOMPARE(reg.planDownloadTotal("app_b"), quint64(6000));
    }

    void clearingTheLastOwnerRemovesTheEntry()
    {
        InstallRegistry reg;
        reg.beginPlan("app_a", {{"shared", "1.0", "H_s", 4000}});
        reg.beginPlan("app_b", {{"shared", "1.0", "H_s", 4000}});

        reg.clearByTopLevel("app_a");
        QVERIFY(reg.has("shared"));
        reg.clearByTopLevel("app_b");
        QVERIFY(!reg.has("shared"));
    }

    // ── AppsModel role surface ──────────────────────────────────────────

    void modelExposesTheProgressRolesByName()
    {
        AppsModel model;
        const QHash<int, QByteArray> names = model.roleNames();
        QCOMPARE(names.value(AppsModel::DownloadReceivedRole),
                 QByteArray("downloadReceived"));
        QCOMPARE(names.value(AppsModel::DownloadTotalRole),
                 QByteArray("downloadTotal"));
    }

    void modelReportsZeroWithNoRegistryAttached()
    {
        AppsModel model;
        model.replaceCatalog({makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui")});
        QCOMPARE(roleOf(model, "wallet_ui", AppsModel::DownloadReceivedRole)
                     .toULongLong(), quint64(0));
        QCOMPARE(roleOf(model, "wallet_ui", AppsModel::DownloadTotalRole)
                     .toULongLong(), quint64(0));
    }

    // The tile binds to these roles, so the aggregate has to survive the
    // trip through the model, not just live in the registry.
    void modelSurfacesTheAggregateNotJustTheAppsOwnBytes()
    {
        InstallRegistry reg;
        AppsModel model;
        model.setInstallRegistry(&reg);
        model.replaceCatalog({makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui")});

        reg.beginPlan("wallet_ui", {{"wallet_ui", "1.0", "H_ui", 1000},
                                    {"wallet_module", "1.0", "H_mod", 4000}});
        reg.setDownloadProgress("wallet_module", 4000, 4000);

        // The PLAN pair is the tile's; it spans the whole install.
        QCOMPARE(roleOf(model, "wallet_ui", AppsModel::PlanDownloadTotalRole)
                     .toULongLong(), quint64(5000));
        QCOMPARE(roleOf(model, "wallet_ui", AppsModel::PlanDownloadReceivedRole)
                     .toULongLong(), quint64(4000));
        // The per-package pair stays the app's own bytes — the dialog lists
        // it beside the dependency, so aggregating here would double-count.
        QCOMPARE(roleOf(model, "wallet_ui", AppsModel::DownloadTotalRole)
                     .toULongLong(), quint64(1000));
        QCOMPARE(roleOf(model, "wallet_ui", AppsModel::DownloadReceivedRole)
                     .toULongLong(), quint64(0));
    }

    void modelSurfacesRegistryProgress()
    {
        InstallRegistry reg;
        AppsModel model;
        model.setInstallRegistry(&reg);
        model.replaceCatalog({makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui")});

        beginDownloading(reg, "wallet_ui");
        reg.setDownloadProgress("wallet_ui", 1024, 4096);

        QCOMPARE(roleOf(model, "wallet_ui", AppsModel::DownloadReceivedRole)
                     .toULongLong(), quint64(1024));
        QCOMPARE(roleOf(model, "wallet_ui", AppsModel::DownloadTotalRole)
                     .toULongLong(), quint64(4096));
    }

    // The row must repaint as bytes arrive, and only the two progress roles
    // should be invalidated — a wider set would churn every binding on the
    // row several times a second.
    void progressEmitsANarrowDataChanged()
    {
        InstallRegistry reg;
        AppsModel model;
        model.setInstallRegistry(&reg);
        model.replaceCatalog({makeCatalogRow("repo1", "wallet_ui", "1.0", "H_ui")});
        beginDownloading(reg, "wallet_ui");

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        reg.setDownloadProgress("wallet_ui", 1024, 4096);

        // Progress may also move the derived session stage, so take the
        // emission that carries the byte roles rather than assuming one.
        QList<int> roles;
        for (const auto& args : spy) {
            const QList<int> r = args.at(2).value<QList<int>>();
            if (r.contains(AppsModel::DownloadReceivedRole)) { roles = r; break; }
        }
        QVERIFY2(!roles.isEmpty(), "no dataChanged carried the progress roles");
        // The per-package pair, the plan pair, and the derived session
        // stage. Still narrow — the point is that a row's other bindings
        // aren't churned several times a second, not the exact count.
        QCOMPARE(roles.size(), 5);
        QVERIFY(roles.contains(AppsModel::DownloadReceivedRole));
        QVERIFY(roles.contains(AppsModel::DownloadTotalRole));
        QVERIFY(roles.contains(AppsModel::PlanDownloadReceivedRole));
        QVERIFY(roles.contains(AppsModel::PlanDownloadTotalRole));
        QVERIFY(roles.contains(AppsModel::PlanInstallStageRole));
        QVERIFY2(!roles.contains(AppsModel::ActionRole),
                 "progress must not invalidate unrelated roles");
    }

    // Two repos can publish the same package name; both rows track the same
    // install, so both must show the bytes.
    void progressReachesEveryRowSharingTheName()
    {
        InstallRegistry reg;
        AppsModel model;
        model.setInstallRegistry(&reg);
        model.replaceCatalog({
            makeCatalogRow("repo1", "wallet_ui", "1.0", "H_shared"),
            makeCatalogRow("repo2", "wallet_ui", "1.0", "H_shared"),
        });
        beginDownloading(reg, "wallet_ui");

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        reg.setDownloadProgress("wallet_ui", 2048, 4096);
        QCOMPARE(spy.count(), 2);
    }
};

QTEST_GUILESS_MAIN(DownloadProgressTest)
#include "download_progress_test.moc"
