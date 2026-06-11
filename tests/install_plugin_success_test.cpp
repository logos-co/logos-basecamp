#include <QtTest/QtTest>
#include <QVariantMap>

static bool installPluginSucceeded(const QVariantMap& r)
{
    return r.value(QStringLiteral("error")).toString().isEmpty();
}

class InstallPluginSuccessTest : public QObject {
    Q_OBJECT
private slots:
    void successResponse_returns_true()
    {
        QVariantMap r;
        r.insert("name",            "wallet_ui");
        r.insert("path",            "/Users/me/.../wallet_ui/wallet_ui.dylib");
        r.insert("isCoreModule",    false);
        r.insert("signatureStatus", "unsigned");
        QVERIFY(installPluginSucceeded(r));
    }

    void uiQml_success_with_empty_path_returns_true()
    {
        QVariantMap r;
        r.insert("name",            "soulseek_ui");
        r.insert("path",            "");
        r.insert("isCoreModule",    false);
        r.insert("signatureStatus", "unsigned");
        QVERIFY(installPluginSucceeded(r));
    }

    void failureResponse_with_nonempty_error_returns_false()
    {
        QVariantMap r;
        r.insert("name",            "soulseek");
        r.insert("path",            "");
        r.insert("isCoreModule",    true);
        r.insert("signatureStatus", "unsigned");
        r.insert("error",
                 "Package does not contain variant for platform: darwin-arm64");
        QVERIFY(!installPluginSucceeded(r));
    }

    // ── Defensive corner cases ──
    void error_key_present_but_empty_string_treated_as_success()
    {
        QVariantMap r;
        r.insert("name", "x"); r.insert("path", "/p"); r.insert("error", "");
        QVERIFY(installPluginSucceeded(r));
    }

    void empty_response_treated_as_success_by_helper_alone()
    {
        QVERIFY(installPluginSucceeded(QVariantMap{}));
    }
};

QTEST_GUILESS_MAIN(InstallPluginSuccessTest)
#include "install_plugin_success_test.moc"
