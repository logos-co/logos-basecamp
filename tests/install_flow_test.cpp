// srcdeps: AppsModel.cpp InstallEnums.cpp InstallRegistry.cpp
//
// End-to-end replay of a real catalog install, asserting the UI-visible
// invariants after EVERY step rather than only at the end.
//
// This exists because two bugs shipped that per-step checks would have
// caught immediately, and neither showed up in the unit tests because those
// tested single transitions in isolation:
//
//   1. Every package in the plan read "Downloading" at once. Each one was
//      promoted on its first bytes and never moved off Downloading, so with
//      three modules all three eventually claimed to be downloading — while
//      package_downloader in fact fetches them strictly one at a time.
//   2. The app's row in the confirm dialog showed the sum of the whole plan
//      instead of its own bytes, because one accessor was serving both the
//      tile (which must aggregate) and the package rows (which must not).
//
// The driver below mirrors what actually happens on the wire:
//   PackageCoordinator::confirmCatalogInstall
//     begin(app)                          -> the app's session
//     beginPlan(app, [deps..., app])      -> resolver order: deps first
//   package_downloader::downloadResolvedDependencies
//     for each package IN ORDER: throttled progress samples to completion
//   the install loop, once every download has finished:
//     setStage(pkg, Installing) -> Installed, per package, in order
//
// checkInvariants() runs after every single mutation. Anything that can only
// be wrong transiently — the simultaneity bug was invisible in any
// end-state assertion — fails here.

#include "AppsModel.h"
#include "InstallEnums.h"
#include "InstallRegistry.h"

#include <QtTest/QtTest>
#include <QVariantList>
#include <QVariantMap>

namespace {

struct Pkg { QString name; quint64 size; };

QVariantMap makeCatalogRow(const QString& repo, const QString& name)
{
    QVariantMap manifest;
    manifest.insert(QStringLiteral("version"),      QStringLiteral("1.0"));
    manifest.insert(QStringLiteral("dependencies"), QVariantList{});
    QVariantMap versionEntry;
    versionEntry.insert(QStringLiteral("rootHash"), QStringLiteral("H_") + name);
    versionEntry.insert(QStringLiteral("manifest"), manifest);
    QVariantMap row;
    row.insert(QStringLiteral("name"),          name);
    row.insert(QStringLiteral("repositoryUrl"), repo);
    row.insert(QStringLiteral("versions"),      QVariantList{versionEntry});
    return row;
}

quint64 roleU64(AppsModel& model, const QString& name, int role)
{
    for (int i = 0; i < model.rowCount(); ++i) {
        const QModelIndex mi = model.index(i);
        if (model.data(mi, AppsModel::NameRole).toString() == name)
            return model.data(mi, role).toULongLong();
    }
    return 0;
}

} // namespace

class InstallFlowTest : public QObject {
    Q_OBJECT

private:
    // The chat plan: two dependencies then the app, the order the resolver
    // emits (dependencies before their consumer).
    QList<Pkg> m_plan{
        {"delivery_module", 3 * 1024 * 1024},
        {"chat_module",     8 * 1024 * 1024},
        {"chat",            1 * 1024 * 1024},
    };
    QString m_app{"chat"};

    quint64 planTotal() const
    {
        quint64 t = 0;
        for (const Pkg& p : m_plan) t += p.size;
        return t;
    }

    // Everything that must hold at EVERY point of the install.
    void checkInvariants(InstallRegistry& reg, AppsModel& model,
                         quint64& highWater, const char* where)
    {
        // (1) Downloads are sequential, so at most one package may read as
        //     Downloading. This is the invariant the simultaneity bug broke.
        int downloading = 0;
        QStringList downloadingNames;
        for (const Pkg& p : m_plan) {
            if (p.name == m_app) continue;   // the app's row tracks the session
            if (reg.stage(p.name) == int(InstallStage::Downloading)) {
                ++downloading;
                downloadingNames << p.name;
            }
        }
        QVERIFY2(downloading <= 1,
                 qPrintable(QStringLiteral("%1: %2 packages report Downloading at once (%3); "
                                           "downloads run one at a time")
                                .arg(where).arg(downloading)
                                .arg(downloadingNames.join(", "))));

        // (2) The tile's aggregate never goes backwards and never exceeds
        //     the plan. A package finishing must not rewind the bar.
        const quint64 agg = roleU64(model, m_app, AppsModel::PlanDownloadReceivedRole);
        QVERIFY2(agg >= highWater,
                 qPrintable(QStringLiteral("%1: aggregate went backwards, %2 -> %3")
                                .arg(where).arg(highWater).arg(agg)));
        highWater = agg;
        QVERIFY2(agg <= planTotal(),
                 qPrintable(QStringLiteral("%1: aggregate %2 exceeds plan total %3")
                                .arg(where).arg(agg).arg(planTotal())));

        // (3) The aggregate IS the sum of the per-package counters — the two
        //     role pairs must never disagree.
        quint64 sum = 0;
        for (const Pkg& p : m_plan)
            sum += roleU64(model, p.name, AppsModel::DownloadReceivedRole);
        QCOMPARE(agg, sum);

        // (4) A package row shows only its own bytes. This is the invariant
        //     the dialog-aggregation bug broke: the app's package row was
        //     reporting the whole plan, double-counting its siblings.
        for (const Pkg& p : m_plan) {
            const quint64 own = roleU64(model, p.name, AppsModel::DownloadReceivedRole);
            QVERIFY2(own <= p.size,
                     qPrintable(QStringLiteral("%1: %2's own bytes (%3) exceed its size (%4) "
                                               "— it is reporting the plan, not itself")
                                    .arg(where, p.name).arg(own).arg(p.size)));
        }

        // (5) The tile's denominator is the whole plan from the first frame,
        //     so the bar never rescales mid-install.
        QCOMPARE(roleU64(model, m_app, AppsModel::PlanDownloadTotalRole), planTotal());
    }

private slots:
    void fullInstallHoldsEveryInvariant()
    {
        InstallRegistry reg;
        AppsModel model;
        model.setInstallRegistry(&reg);

        QVariantList catalog;
        for (const Pkg& p : m_plan) catalog.append(makeCatalogRow("repo1", p.name));
        model.replaceCatalog(catalog);

        quint64 highWater = 0;

        // ── confirmCatalogInstall ───────────────────────────────────────
        QList<InstallRegistry::PlannedPackage> plan;
        for (const Pkg& p : m_plan)
            plan.append({p.name, "1.0", "H_" + p.name, p.size});
        reg.beginPlan(m_app, plan);
        checkInvariants(reg, model, highWater, "after beginPlan");

        // Nothing is on the wire yet, so every package row reads Queued —
        // including the app's own, whose package downloads last. The TILE
        // meanwhile reads Downloading, because the install is under way.
        for (const Pkg& p : m_plan)
            QCOMPARE(reg.stage(p.name), int(InstallStage::Queued));

        // Nothing has downloaded yet, but the total is already the plan's.
        QCOMPARE(roleU64(model, m_app, AppsModel::PlanDownloadReceivedRole), quint64(0));
        QCOMPARE(roleU64(model, m_app, AppsModel::PlanDownloadTotalRole), planTotal());

        // ── the sequential download loop ────────────────────────────────
        for (const Pkg& p : m_plan) {
            // Throttled samples: first, a few mid-transfer, then completion.
            const QList<quint64> marks{0, p.size / 4, p.size / 2,
                                       (p.size * 3) / 4, p.size};
            for (quint64 got : marks) {
                reg.setDownloadProgress(p.name, got, p.size);
                checkInvariants(reg, model, highWater,
                                qPrintable(QStringLiteral("downloading %1").arg(p.name)));
            }
            // Its bytes are all in, so it is no longer on the wire.
            QCOMPARE(reg.downloadReceived(p.name), p.size);
            QVERIFY2(reg.stage(p.name) == int(InstallStage::Downloaded),
                     qPrintable(QStringLiteral("%1 finished but reads stage %2 — a completed "
                                               "download must move FORWARD to Downloaded, "
                                               "never back to Queued")
                                    .arg(p.name).arg(reg.stage(p.name))));
        }

        // Every byte accounted for.
        QCOMPARE(roleU64(model, m_app, AppsModel::PlanDownloadReceivedRole), planTotal());

        // ── the install loop ────────────────────────────────────────────
        for (const Pkg& p : m_plan) {
            reg.setStage(p.name, InstallStage::Installing);
            checkInvariants(reg, model, highWater,
                            qPrintable(QStringLiteral("installing %1").arg(p.name)));
            reg.setStage(p.name, InstallStage::Installed);
            checkInvariants(reg, model, highWater,
                            qPrintable(QStringLiteral("installed %1").arg(p.name)));
        }

        // Installing must not have rewound the tile — the counters are the
        // record of the transfer, not live state.
        QCOMPARE(roleU64(model, m_app, AppsModel::PlanDownloadReceivedRole), planTotal());

        // ── teardown ────────────────────────────────────────────────────
        reg.clearByTopLevel(m_app);
        for (const Pkg& p : m_plan)
            QVERIFY2(!reg.has(p.name),
                     qPrintable(QStringLiteral("%1 outlived the install").arg(p.name)));
        QCOMPARE(roleU64(model, m_app, AppsModel::PlanDownloadReceivedRole), quint64(0));
    }

    // The tile shows ONE continuous download phase, then installing. It must
    // never flap back to a bar-less state and then to Downloading again:
    // packages are fetched one at a time, so between them there is a moment
    // when nothing is on the wire, and treating that as its own session
    // state made the tile blink "Installing…" between every package.
    void sessionPhaseNeverFlapsBackToDownloading()
    {
        InstallRegistry reg;
        AppsModel model;
        model.setInstallRegistry(&reg);
        QVariantList catalog;
        for (const Pkg& p : m_plan) catalog.append(makeCatalogRow("repo1", p.name));
        model.replaceCatalog(catalog);

        QList<InstallRegistry::PlannedPackage> plan;
        for (const Pkg& p : m_plan)
            plan.append({p.name, "1.0", "H_" + p.name, p.size});
        reg.beginPlan(m_app, plan);

        QList<int> phases;
        const auto sample = [&]() {
            const int st = int(roleU64(model, m_app, AppsModel::PlanInstallStageRole));
            if (phases.isEmpty() || phases.last() != st) phases.append(st);
        };

        sample();
        for (const Pkg& p : m_plan) {
            reg.setDownloadProgress(p.name, 0, p.size);          sample();
            reg.setDownloadProgress(p.name, p.size / 2, p.size); sample();
            reg.setDownloadProgress(p.name, p.size, p.size);     sample();
            // The inter-package gap: the finished one is Downloaded and the
            // next has not started. This is where the blink happened.
            sample();
        }
        for (const Pkg& p : m_plan) {
            reg.setStage(p.name, InstallStage::Installing); sample();
            reg.setStage(p.name, InstallStage::Installed);  sample();
        }

        // Exactly three phases, in order, each entered once.
        const QList<int> expected{int(InstallStage::Downloading),
                                  int(InstallStage::Installing),
                                  int(InstallStage::Installed)};
        QCOMPARE(phases, expected);
    }

    // The sequence as watched on screen, asserted step by step:
    // press install -> every row Queued, tile Downloading -> delivery
    // downloads -> delivery Downloaded (NOT back to Queued) -> chat_module
    // downloads -> then the app itself. No row ever moves backwards.
    void watchedSequenceNeverMovesARowBackwards()
    {
        InstallRegistry reg;
        AppsModel model;
        model.setInstallRegistry(&reg);
        QVariantList catalog;
        for (const Pkg& p : m_plan) catalog.append(makeCatalogRow("repo1", p.name));
        model.replaceCatalog(catalog);

        QList<InstallRegistry::PlannedPackage> plan;
        for (const Pkg& p : m_plan)
            plan.append({p.name, "1.0", "H_" + p.name, p.size});
        reg.beginPlan(m_app, plan);

        // Press install: nothing on the wire, so no package claims to be
        // downloading — the app included. Its row must NOT show a 0-of-its-
        // own-size bar while its dependencies are the ones being fetched.
        for (const Pkg& p : m_plan)
            QCOMPARE(reg.stage(p.name), int(InstallStage::Queued));
        QCOMPARE(roleU64(model, m_app, AppsModel::DownloadReceivedRole), quint64(0));
        // The session is in its download phase from the moment the plan is
        // registered, and stays there until something installs — one
        // continuous phase, no flicker between packages.
        QCOMPARE(int(roleU64(model, m_app, AppsModel::PlanInstallStageRole)),
                 int(InstallStage::Downloading));

        // A stage may only advance. Track the furthest each row has reached.
        QHash<QString, int> furthest;
        const auto rank = [](int stage) {
            switch (stage) {
            case InstallStage::Queued:      return 0;
            case InstallStage::Downloading: return 1;
            case InstallStage::Downloaded:  return 2;
            case InstallStage::Installing:  return 3;
            case InstallStage::Installed:   return 4;
            }
            return -1;
        };
        const auto noRegression = [&](const char* where) {
            for (const Pkg& p : m_plan) {
                const int r = rank(reg.stage(p.name));
                QVERIFY2(r >= furthest.value(p.name, 0),
                         qPrintable(QStringLiteral("%1: %2 moved backwards to stage %3")
                                        .arg(where, p.name).arg(reg.stage(p.name))));
                furthest[p.name] = r;
            }
        };

        for (const Pkg& p : m_plan) {
            reg.setDownloadProgress(p.name, 0, p.size);
            QCOMPARE(reg.stage(p.name), int(InstallStage::Downloading));
            // With a package on the wire the session reads Downloading, so
            // the tile draws its bar.
            QCOMPARE(int(roleU64(model, m_app, AppsModel::PlanInstallStageRole)),
                     int(InstallStage::Downloading));
            noRegression("mid-download");

            reg.setDownloadProgress(p.name, p.size, p.size);
            QCOMPARE(reg.stage(p.name), int(InstallStage::Downloaded));
            noRegression("download complete");
        }

        // Every download done but nothing installing yet: still the download
        // phase, bar full, about to hand over.
        QCOMPARE(int(roleU64(model, m_app, AppsModel::PlanInstallStageRole)),
                 int(InstallStage::Downloading));

        for (const Pkg& p : m_plan) {
            reg.setStage(p.name, InstallStage::Installing);
            noRegression("installing");
            reg.setStage(p.name, InstallStage::Installed);
            noRegression("installed");
        }
        QCOMPARE(int(roleU64(model, m_app, AppsModel::PlanInstallStageRole)),
                 int(InstallStage::Installed));
    }

    // The bug as the user saw it: watching a three-module install, more than
    // one row claimed to be downloading. Pinned on its own so a regression
    // names the actual symptom.
    void onlyOnePackageEverReadsAsDownloading()
    {
        InstallRegistry reg;
        QList<InstallRegistry::PlannedPackage> plan;
        for (const Pkg& p : m_plan)
            plan.append({p.name, "1.0", "H_" + p.name, p.size});
        reg.beginPlan(m_app, plan);

        int peak = 0;
        for (const Pkg& p : m_plan) {
            for (quint64 got : {quint64(0), p.size / 2, p.size}) {
                reg.setDownloadProgress(p.name, got, p.size);
                int n = 0;
                for (const Pkg& q : m_plan)
                    if (q.name != m_app
                        && reg.stage(q.name) == int(InstallStage::Downloading)) ++n;
                peak = qMax(peak, n);
            }
        }
        QCOMPARE(peak, 1);
    }

    // A finished package waits its turn to install rather than continuing to
    // advertise a transfer, and cannot be dragged back by a late sample.
    void aFinishedPackageStaysOffTheWire()
    {
        InstallRegistry reg;
        reg.beginPlan(m_app, {{"delivery_module", "1.0", "H_d", 1000}});
        reg.setDownloadProgress("delivery_module", 1000, 1000);
        QCOMPARE(reg.stage("delivery_module"), int(InstallStage::Downloaded));

        // A duplicate sample arriving late (the event crosses a process
        // boundary) must not restart it.
        reg.setDownloadProgress("delivery_module", 1000, 1000);
        QCOMPARE(reg.stage("delivery_module"), int(InstallStage::Downloaded));
        QCOMPARE(reg.downloadReceived("delivery_module"), quint64(1000));
    }

    // The two role pairs answer different questions and must not be swapped.
    void packageRowAndTileReportDifferentNumbers()
    {
        InstallRegistry reg;
        AppsModel model;
        model.setInstallRegistry(&reg);
        QVariantList catalog;
        for (const Pkg& p : m_plan) catalog.append(makeCatalogRow("repo1", p.name));
        model.replaceCatalog(catalog);

        QList<InstallRegistry::PlannedPackage> plan;
        for (const Pkg& p : m_plan)
            plan.append({p.name, "1.0", "H_" + p.name, p.size});
        reg.beginPlan(m_app, plan);

        reg.setDownloadProgress("delivery_module", 3 * 1024 * 1024, 3 * 1024 * 1024);
        reg.setDownloadProgress("chat_module", 4 * 1024 * 1024, 8 * 1024 * 1024);

        // The app's PACKAGE row: it has not downloaded a byte of itself yet.
        QCOMPARE(roleU64(model, m_app, AppsModel::DownloadReceivedRole), quint64(0));
        QCOMPARE(roleU64(model, m_app, AppsModel::DownloadTotalRole),
                 quint64(1 * 1024 * 1024));
        // The app's TILE: everything fetched so far, across the plan.
        QCOMPARE(roleU64(model, m_app, AppsModel::PlanDownloadReceivedRole),
                 quint64(7 * 1024 * 1024));
        QCOMPARE(roleU64(model, m_app, AppsModel::PlanDownloadTotalRole), planTotal());
    }
};

QTEST_GUILESS_MAIN(InstallFlowTest)
#include "install_flow_test.moc"
