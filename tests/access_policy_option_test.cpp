// srcdeps: ../app/utils/AccessPolicyOption.cpp
//
// Unit tests for Basecamp's --access-policy resolution (app/utils/
// AccessPolicyOption.{h,cpp}) — the opt-in half of deny-by-default inter-module
// enforcement.
//
// Basecamp's default is, and must remain, "no policy": main.cpp calls
// logos_core_set_access_policy(nullptr) unless something here produced a
// document. The first test is therefore the load-bearing one — several modules
// in this tree (out-of-process ui_qml plugins especially) call targets they
// never declared, and flipping the default would break them.
//
// What the resolved document then MEANS is the runtime's business:
// `mode: "enforce"` is what arms deny-by-default (liblogos access_policy.h),
// which is why these tests assert on the parsed `mode` rather than on an exact
// byte string. Run: nix build .#unit-tests -L

#include "../app/utils/AccessPolicyOption.h"

#include <QtTest/QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using LogosBasecamp::resolveAccessPolicy;

namespace {

// The `mode` the runtime would read out of a resolution, or a null QString
// when the resolution installs no policy at all.
QString modeOf(const LogosBasecamp::AccessPolicyResolution& r)
{
    if (r.policyJson.isEmpty()) return QString();
    return QJsonDocument::fromJson(r.policyJson.toUtf8())
        .object()
        .value(QStringLiteral("mode"))
        .toString();
}

} // namespace

class AccessPolicyOptionTest : public QObject
{
    Q_OBJECT

private slots:
    // ── Flag absent: today's behaviour, exactly ─────────────────────────────

    void noArgumentInstallsNoPolicy()
    {
        for (const QString& arg : {QString(), QStringLiteral(""), QStringLiteral("   ")}) {
            const auto r = resolveAccessPolicy(arg);
            QVERIFY2(r.ok, qPrintable(r.error));
            QVERIFY2(r.policyJson.isEmpty(),
                     "no --access-policy must resolve to NO policy: main.cpp then "
                     "passes nullptr and enforcement stays off, which is the "
                     "pre-existing behaviour");
        }
    }

    // ── The deny-by-default opt-in ──────────────────────────────────────────

    void enforceAliasArmsEnforceMode()
    {
        const auto r = resolveAccessPolicy(QStringLiteral("enforce"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!r.policyJson.isEmpty());
        // `mode` is the runtime's switch; only "enforce" turns denials on.
        QCOMPARE(modeOf(r), QStringLiteral("enforce"));

        // No explicit restrictions: the runtime derives them from the declared
        // dependency graph, which is what deny-by-default means here.
        const QJsonObject doc = QJsonDocument::fromJson(r.policyJson.toUtf8()).object();
        QVERIFY(doc.value(QStringLiteral("restrictions")).toObject().isEmpty());
    }

    void enforceAliasIsNotReadAsAFilePath()
    {
        // A readable file named `enforce` in the working directory must not
        // hijack the alias — otherwise arming enforcement would depend on where
        // the app was launched from.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile f(dir.filePath(QStringLiteral("enforce")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"version":1,"mode":"audit"})");
        f.close();

        const QString prev = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));
        const auto r = resolveAccessPolicy(QStringLiteral("enforce"));
        QDir::setCurrent(prev);

        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(modeOf(r), QStringLiteral("enforce"));
    }

    // ── Full policy documents ───────────────────────────────────────────────

    void inlineJsonIsPassedThrough()
    {
        // The escape hatch for the ui_qml problem: an explicit entry replaces
        // the derived allow-list for that target.
        const QString inlineDoc = QStringLiteral(
            R"({"version":1,"mode":"enforce","restrictions":)"
            R"({"accounts_module":{"allowedCallers":["accounts_ui"]}}})");
        const auto r = resolveAccessPolicy(inlineDoc);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.policyJson, inlineDoc);

        const QJsonObject restrictions =
            QJsonDocument::fromJson(r.policyJson.toUtf8())
                .object()
                .value(QStringLiteral("restrictions"))
                .toObject();
        QVERIFY(restrictions.contains(QStringLiteral("accounts_module")));
    }

    void filePathIsReadFromDisk()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("policy.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"version":1,"mode":"enforce","restrictions":{}})");
        f.close();

        const auto r = resolveAccessPolicy(path);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(modeOf(r), QStringLiteral("enforce"));
    }

    void nonEnforceModeIsResolvedButLeavesEnforcementOff()
    {
        // Resolution is not the gate: this document is handed to the runtime
        // verbatim, and the runtime declines to enforce anything but "enforce".
        const auto r = resolveAccessPolicy(QStringLiteral(R"({"version":1,"mode":"audit"})"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(modeOf(r), QStringLiteral("audit"));
    }

    // ── Operator errors are loud, never a silent fallback to "off" ──────────

    void missingFileFailsWithAReason()
    {
        const auto r = resolveAccessPolicy(QStringLiteral("/definitely/not/here/policy.json"));
        QVERIFY2(!r.ok, "an unreadable policy file must fail, not silently boot wide open");
        QVERIFY(r.error.contains(QStringLiteral("could not be opened")));
        QVERIFY(r.policyJson.isEmpty());
    }

    void malformedInlineJsonFailsWithAReason()
    {
        const auto r = resolveAccessPolicy(QStringLiteral("{not valid json"));
        QVERIFY2(!r.ok, "malformed JSON must fail, not silently boot wide open");
        QVERIFY(r.error.contains(QStringLiteral("not valid JSON")));
    }

    void malformedFileJsonFailsWithAReason()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("bad.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{oops");
        f.close();

        const auto r = resolveAccessPolicy(path);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("not valid JSON")));
    }
};

QTEST_GUILESS_MAIN(AccessPolicyOptionTest)
#include "access_policy_option_test.moc"
