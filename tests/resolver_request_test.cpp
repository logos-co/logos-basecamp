// Regression guard for the install gate's dependency-resolution request.
//
// The gate resolves dependencies independently of package_manager_ui, which
// performs the install. For its dialog to describe what actually happens, both
// must ask the resolver the same question — and PMUI asks by naming ONE
// package and letting the resolver walk the rest.
//
// The gate briefly reused PackageCoordinator::buildResolverDepsJson, which
// pre-expands every dependency into the request for the App-Manager's
// per-dependency version pins. Everything in the request returns
// `topLevel: true`, and the gate filters top-level entries out as the subject
// of its own dialog — so every dependency vanished, the dialog stated
// "No other packages need to change", and the installer installed them anyway.

#include "ResolverRequest.h"

#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

class ResolverRequestTest : public QObject {
    Q_OBJECT

private:
    static QJsonArray parse(const QString& json)
    {
        return QJsonDocument::fromJson(json.toUtf8()).array();
    }

private slots:
    // The property the whole fix rests on: one entry, the subject only. Any
    // dependency added here would come back top-level and be filtered away.
    void requestNamesOnlyTheSubject()
    {
        const QJsonArray arr = parse(
            logos::gateResolverRequest("chat", "https://repo/logos-repo.json", "1.0.0"));

        QCOMPARE(arr.size(), 1);
        const QJsonObject o = arr.at(0).toObject();
        QCOMPARE(o.value("name").toString(), QStringLiteral("chat"));
        QCOMPARE(o.value("repositoryUrl").toString(),
                 QStringLiteral("https://repo/logos-repo.json"));
        QCOMPARE(o.value("version").toString(), QStringLiteral("1.0.0"));
    }

    // A dependency must never appear. If it does, the resolver marks it
    // top-level and the gate silently drops it — the exact regression.
    void requestNeverCarriesADependency()
    {
        const QJsonArray arr = parse(
            logos::gateResolverRequest("chat", "https://repo/logos-repo.json", "1.0.0"));
        for (const QJsonValue& v : arr) {
            const QString n = v.toObject().value("name").toString();
            QVERIFY2(n == QLatin1String("chat"),
                     qPrintable(QStringLiteral("request carries '%1' besides the subject; "
                                               "it would return topLevel and be filtered out")
                                    .arg(n)));
        }
    }

    // The installed set the gate resolves against is shaped from a list it
    // fetched, not from a cache — so an uninstalled package must be absent
    // the moment package_manager stops reporting it. When it lingered, the
    // resolver treated the dependency as satisfied and the gate reported no
    // changes while the installer installed it.
    void installedSetIsShapedFromTheGivenList()
    {
        QVariantMap chat;
        chat.insert("moduleName", "chat_module");
        chat.insert("version", "2.0.0");
        QVariantMap hashes; hashes.insert("root", "h_chat");
        chat.insert("hashes", hashes);

        const QJsonArray arr = parse(
            logos::installedPackagesJson(QVariantList{chat}));
        QCOMPARE(arr.size(), 1);
        QCOMPARE(arr.at(0).toObject().value("name").toString(),
                 QStringLiteral("chat_module"));
        QCOMPARE(arr.at(0).toObject().value("rootHash").toString(),
                 QStringLiteral("h_chat"));

        // The uninstalled case: an empty list must shape to an empty set, so
        // nothing is reported as already satisfied.
        QCOMPARE(parse(logos::installedPackagesJson({})).size(), 0);
    }

    // Entries without a usable name or version are dropped rather than sent
    // half-formed — the resolver keys its short-circuit on both.
    void incompleteInstalledEntriesAreDropped()
    {
        QVariantMap noVersion; noVersion.insert("moduleName", "x");
        QVariantMap noName;    noName.insert("version", "1.0.0");
        QCOMPARE(parse(logos::installedPackagesJson(
                     QVariantList{noVersion, noName})).size(), 0);
    }

    // Optional fields are omitted rather than sent empty — an empty
    // repositoryUrl would scope the resolver to a repo that does not exist.
    void emptyOptionalFieldsAreOmitted()
    {
        const QJsonObject o = parse(logos::gateResolverRequest("chat", "", "")).at(0).toObject();
        QCOMPARE(o.value("name").toString(), QStringLiteral("chat"));
        QVERIFY(!o.contains("repositoryUrl"));
        QVERIFY(!o.contains("version"));
    }
};

QTEST_GUILESS_MAIN(ResolverRequestTest)
#include "resolver_request_test.moc"
