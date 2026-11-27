// basecamp#333: muster_ui's repository didn't provide lez_core, so the
// installed 0.4.0, a different build from the one it was made with, was
// used, and nothing said so.

#include "utils/OutsideDependencies.h"

#include <QTest>

using namespace logos::outside;

namespace {

QVariantMap row(const QString& name, const QString& version, const QString& repo,
                const QVariantMap& graph = {})
{
    QVariantMap r{{"name", name}, {"version", version}, {"rootHash", name + "-hash"},
                  {"repositoryUrl", repo}};
    if (!graph.isEmpty()) {
        r.insert("topLevel", true);
        r.insert("dependencyGraph", graph);
    }
    return r;
}

Listed listedIn(const QStringList& names)
{
    return [names](const QString& n) { return names.contains(n); };
}

Installed installedAs(const QMap<QString, QString>& versions)
{
    return [versions](const QString& n) {
        return qMakePair(versions.value(n), versions.contains(n) ? n + "-installed" : QString());
    };
}

const QString kAppRepo = "https://apps.example";
const QString kLogos = "https://logos.example";

}  // namespace

class TestOutsideDependencies : public QObject {
    Q_OBJECT

private slots:
    // The resolver kept the installed copy, so the plan has no row for it.
    void anInstalledCopyTheAppsRepositoryDoesntProvide()
    {
        const QVariantList plan{row("muster_ui", "1.0.0", kAppRepo,
                                    {{"muster_ui", QVariantList{"lez_core"}}})};
        const auto deps = boundDependencies("muster_ui", plan, listedIn({"muster_ui"}),
                                            installedAs({{"lez_core", "0.4.0"}}));
        QCOMPARE(deps.size(), 1);
        QCOMPARE(deps[0].name, QString("lez_core"));
        QCOMPARE(deps[0].requiredBy, QString("muster_ui"));
        QCOMPARE(deps[0].version, QString("0.4.0"));
        QCOMPARE(deps[0].rootHash, QString("lez_core-installed"));
        QVERIFY(deps[0].installedCopy);
        QVERIFY(deps[0].outside);

        const QVariantList shown = forDialog(deps, [](const QString&) { return QString("unused"); });
        QCOMPARE(shown.size(), 1);
        QCOMPARE(shown[0].toMap().value("installed").toBool(), true);
        QCOMPARE(shown[0].toMap().value("version").toString(), QString("0.4.0"));
        QVERIFY(shown[0].toMap().value("repository").toString().isEmpty());

        const QStringList log = logLines("muster_ui", kAppRepo, deps);
        QCOMPARE(log.size(), 1);
        QVERIFY(log[0].contains("lez_core 0.4.0 (rootHash lez_core-installed)"));
        QVERIFY(log[0].contains("not provided by " + kAppRepo));
    }

    void aDependencyTakenFromAnotherRepository()
    {
        const QVariantList plan{row("lez_core", "0.4.1", kLogos),
                                row("muster_ui", "1.0.0", kAppRepo,
                                    {{"muster_ui", QVariantList{QVariantMap{{"name", "lez_core"}}}},
                                     {"lez_core", QVariantList{}}})};
        const auto deps = boundDependencies("muster_ui", plan, listedIn({"muster_ui"}), installedAs({}));
        QCOMPARE(deps.size(), 1);
        QVERIFY(!deps[0].installedCopy);
        QVERIFY(deps[0].outside);
        QCOMPARE(deps[0].repositoryUrl, kLogos);

        const QVariantList shown = forDialog(deps, [](const QString& url) {
            return url == kLogos ? QString("Logos official") : url;
        });
        QCOMPARE(shown.size(), 1);
        QCOMPARE(shown[0].toMap().value("installed").toBool(), false);
        QCOMPARE(shown[0].toMap().value("repository").toString(), QString("Logos official"));
        QVERIFY(logLines("muster_ui", kAppRepo, deps)[0].contains("; from " + kLogos));
    }

    // Its repository can pin it: no notice, though an installed copy is still logged.
    void aDependencyTheAppsRepositoryProvides()
    {
        const QVariantList plan{row("muster_ui", "1.0.0", kAppRepo,
                                    {{"muster_ui", QVariantList{"lez_core", "signer"}}}),
                                row("signer", "2.0.0", kAppRepo)};
        const auto deps = boundDependencies("muster_ui", plan, listedIn({"muster_ui", "lez_core", "signer"}),
                                            installedAs({{"lez_core", "0.4.0"}}));
        QCOMPARE(deps.size(), 2);
        QVERIFY(!deps[0].outside && !deps[1].outside);
        QVERIFY(forDialog(deps, [](const QString& u) { return u; }).isEmpty());
        const QStringList log = logLines("muster_ui", kAppRepo, deps);
        QCOMPARE(log.size(), 1);
        QVERIFY(log[0].contains("lez_core 0.4.0") && !log[0].contains("not provided"));
    }

    // Neither planned nor installed: the resolver refuses that plan, so there
    // is nothing to bind.
    void aDependencyNobodyHasIsNotReported()
    {
        const QVariantList plan{row("muster_ui", "1.0.0", kAppRepo,
                                    {{"muster_ui", QVariantList{"lez_core"}}})};
        QVERIFY(boundDependencies("muster_ui", plan, listedIn({"muster_ui"}), installedAs({})).isEmpty());
    }

    // What a planned dependency needs is walked; what an installed copy needs
    // was bound when it was installed.
    void walksWhatThePlanDownloadsOnly()
    {
        const QVariantList plan{row("wallet", "1.0.0", kLogos),
                                row("muster_ui", "1.0.0", kAppRepo,
                                    {{"muster_ui", QVariantList{"wallet", "lez_core"}},
                                     {"wallet", QVariantList{"keys"}},
                                     {"lez_core", QVariantList{"ffi"}}})};
        const auto deps = boundDependencies("muster_ui", plan, listedIn({"muster_ui"}),
                                            installedAs({{"lez_core", "0.4.0"}, {"keys", "1.0.0"},
                                                         {"ffi", "1.0.0"}}));
        QStringList names;
        for (const auto& d : deps) names << d.name;
        QCOMPARE(names, (QStringList{"wallet", "lez_core", "keys"}));
        QCOMPARE(deps[2].requiredBy, QString("wallet"));
    }
};

QTEST_GUILESS_MAIN(TestOutsideDependencies)
#include "outside_dependencies_test.moc"
