#include <QtTest/QtTest>

#include <QJsonObject>
#include <QVariantHash>
#include <QVariantMap>

#include "utils/DependencyEntry.h"

using logos::DependencyEntryKind;
using logos::readDependencyEntry;

// A manifest `dependencies[]` entry is either a bare name ("wallet_module")
// or an object carrying that name alongside the constraints an installer
// resolves it by: {"name": …, "version": "^2.0.0", "signer": "did:jwk:…"}.
// Both forms declare THE SAME EDGE, so both must yield the same name here.
//
// The trap: QVariant::toString() on a QVariantMap returns a NULL QString, not
// a diagnostic. A reader written as
//
//     QString depName = dep.toString();
//     if (depName.isEmpty()) continue;
//
// therefore drops every object-form entry in total silence — no error, no log
// line — and the plugin mounts with an unloaded dependency.
class DependencyEntryTest : public QObject {
    Q_OBJECT

private slots:
    // ── The Qt semantics this whole file exists for ─────────────────────
    void toString_on_an_object_entry_is_null_not_a_diagnostic()
    {
        QVariantMap constrained;
        constrained.insert("name",    "wallet_module");
        constrained.insert("version", "^2.0.0");
        constrained.insert("signer",  "did:jwk:eyJrdHkiOiJPS1AifQ");

        // This is why a bare toString() reader cannot be trusted: the call
        // succeeds, returns nothing, and reports nothing.
        QVERIFY(QVariant(constrained).toString().isNull());
    }

    // ── Bare-name entries: the shape the ABI sends today ────────────────
    void bare_name_yields_that_name()
    {
        const auto e = readDependencyEntry(QVariant(QStringLiteral("wallet_module")));
        QCOMPARE(e.kind, DependencyEntryKind::Name);
        QCOMPARE(e.name, QStringLiteral("wallet_module"));
    }

    // ── Object entries: the shape the ABI will send once widened ────────
    void object_entry_yields_its_name()
    {
        QVariantMap constrained;
        constrained.insert("name",    "wallet_module");
        constrained.insert("version", "^2.0.0");
        constrained.insert("signer",  "did:jwk:eyJrdHkiOiJPS1AifQ");

        const auto e = readDependencyEntry(QVariant(constrained));
        QCOMPARE(e.kind, DependencyEntryKind::Name);
        QCOMPARE(e.name, QStringLiteral("wallet_module"));
    }

    void object_entry_without_constraints_yields_its_name()
    {
        QVariantMap bare;
        bare.insert("name", "libp2p_module");

        const auto e = readDependencyEntry(QVariant(bare));
        QCOMPARE(e.kind, DependencyEntryKind::Name);
        QCOMPARE(e.name, QStringLiteral("libp2p_module"));
    }

    // The IPC layer hands us QVariantMap, but a caller that went through
    // QJsonDocument directly holds a QJsonObject. Same entry, same answer.
    void json_object_entry_yields_its_name()
    {
        QJsonObject o;
        o.insert("name",    "chat_module");
        o.insert("version", ">=1.2.0 <2.0.0");

        const auto e = readDependencyEntry(QVariant(o));
        QCOMPARE(e.kind, DependencyEntryKind::Name);
        QCOMPARE(e.name, QStringLiteral("chat_module"));
    }

    void hash_object_entry_yields_its_name()
    {
        QVariantHash h;
        h.insert("name", "waku_module");

        const auto e = readDependencyEntry(QVariant(h));
        QCOMPARE(e.kind, DependencyEntryKind::Name);
        QCOMPARE(e.name, QStringLiteral("waku_module"));
    }

    // ── Everything we cannot name is Unrecognised, never a quiet skip ───
    void object_without_a_name_is_unrecognised()
    {
        QVariantMap noName;
        noName.insert("version", "^2.0.0");

        const auto e = readDependencyEntry(QVariant(noName));
        QCOMPARE(e.kind, DependencyEntryKind::Unrecognised);
        QVERIFY(e.name.isEmpty());
    }

    void object_with_an_empty_name_is_unrecognised()
    {
        QVariantMap emptyName;
        emptyName.insert("name",    "");
        emptyName.insert("version", "^2.0.0");

        const auto e = readDependencyEntry(QVariant(emptyName));
        QCOMPARE(e.kind, DependencyEntryKind::Unrecognised);
    }

    void empty_string_is_unrecognised()
    {
        QCOMPARE(readDependencyEntry(QVariant(QString())).kind,
                 DependencyEntryKind::Unrecognised);
        QCOMPARE(readDependencyEntry(QVariant(QStringLiteral(""))).kind,
                 DependencyEntryKind::Unrecognised);
    }

    void a_nested_list_is_unrecognised()
    {
        const QVariantList nested{QStringLiteral("wallet_module")};
        QCOMPARE(readDependencyEntry(QVariant(nested)).kind,
                 DependencyEntryKind::Unrecognised);
    }

    void a_number_is_unrecognised()
    {
        // Not "42": a dependencies[] entry is a name or an object, and a
        // number is neither. Reporting it beats silently loading a module
        // called "42" and beats silently loading nothing.
        QCOMPARE(readDependencyEntry(QVariant(42)).kind,
                 DependencyEntryKind::Unrecognised);
    }

    void an_invalid_variant_is_unrecognised()
    {
        QCOMPARE(readDependencyEntry(QVariant()).kind,
                 DependencyEntryKind::Unrecognised);
    }
};

QTEST_MAIN(DependencyEntryTest)
#include "dependency_entry_test.moc"
