#include "InstalledIntegrity.h"

#include <lgx.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace logos::integrity {

Problem examine(const QString& packageDir)
{
    using Kind = Problem::Kind;
    const QDir dir(packageDir);
    if (!dir.exists())
        return {QStringLiteral("the installed package is missing"), Kind::Unverifiable};

    QFile manifestFile(dir.filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::ReadOnly))
        return {QStringLiteral("its manifest.json is missing"), Kind::Unverifiable};
    const QByteArray manifest = manifestFile.readAll();
    QFile variantFile(dir.filePath(QStringLiteral("variant")));
    if (!variantFile.open(QIODevice::ReadOnly))
        return {QStringLiteral("its variant file is missing"), Kind::Unverifiable};
    const QByteArray variant = variantFile.readAll().trimmed();

    // liblgx makes a std::filesystem::path of this char*, which Windows reads
    // in the ANSI code page, not UTF-8.
#ifdef Q_OS_WIN
    const QByteArray nativeDir = packageDir.toLocal8Bit();
#else
    const QByteArray nativeDir = QFile::encodeName(packageDir);
#endif
    const lgx_integrity_t integrity = lgx_verify_installed_tree(
        nativeDir.constData(), manifest.constData(),
        static_cast<size_t>(manifest.size()), variant.constData());
    switch (integrity) {
    case LGX_INTEGRITY_OK:
        break;
    case LGX_INTEGRITY_MISMATCH:
        return {QStringLiteral("its files were modified or damaged since it was installed"), Kind::Changed};
    case LGX_INTEGRITY_NO_HASH:
        return {QStringLiteral("its manifest carries no content hashes"), Kind::Unverifiable};
    default:
        return {QStringLiteral("it could not be checked: ") + QString::fromUtf8(lgx_get_last_error()),
                Kind::Unchecked};
    }

    // The hashes only prove the files match the manifest beside them; for a
    // signed package, its signature proves that manifest was not rewritten.
    // The signer is the one manifest.sig names: this asks whether the manifest
    // is still the one that was signed, not whether to trust who signed it.
    QFile sigFile(dir.filePath(QStringLiteral("manifest.sig")));
    if (!sigFile.exists())
        return {};
    if (!sigFile.open(QIODevice::ReadOnly))
        return {QStringLiteral("its signature file could not be read"), Kind::Unchecked};
    const QByteArray sig = sigFile.readAll();
    const QByteArray signer =
        QJsonDocument::fromJson(sig).object().value(QStringLiteral("did")).toString().toUtf8();
    if (signer.isEmpty())
        return {QStringLiteral("its signature file is unreadable"), Kind::Unverifiable};
    if (lgx_check_manifest_signature(manifest.constData(), static_cast<size_t>(manifest.size()),
                                     sig.constData(), signer.constData()) != LGX_SIG_CHECK_OK)
        return {QStringLiteral("its signature no longer matches its manifest"), Kind::Changed};
    return {};
}

Problem problemWithAny(const QStringList& paths, const QString& installRoot,
                       QString* failedPackageDir)
{
    return problemWithAny(paths, installRoot, &examine, failedPackageDir);
}

Preflight preflight(const ICoreRuntime& core, const QString& moduleName,
                    const QString& installRoot)
{
    return preflight(core, moduleName, installRoot, &examine);
}

void preflightInBackground(const QString& modulesInfoJson, const QString& moduleName,
                           const QString& installRoot, QObject* context,
                           std::function<void(const Preflight&)> done)
{
    preflightInBackground(modulesInfoJson, moduleName, installRoot, &examine, context,
                          std::move(done));
}

QString copyThatRuns(const QString& name, const QStringList& modulesDirs)
{
    QString winner;
    QByteArray winnerVersion;
    for (const QString& modulesDir : modulesDirs) {
        if (modulesDir.isEmpty())
            continue;
        const QString dir = QDir(modulesDir).filePath(name);
        QFile file(QDir(dir).filePath(QStringLiteral("manifest.json")));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object();
        if (manifest.value(QStringLiteral("name")).toString() != name
            || manifest.value(QStringLiteral("type")).toString() != QLatin1String("core"))
            continue;
        const QByteArray version = manifest.value(QStringLiteral("version")).toString().toUtf8();
        if (!winner.isEmpty() && lgx_semver_compare(winnerVersion.constData(), version.constData()) > 0)
            continue;
        winner = dir;
        winnerVersion = version;
    }
    return winner;
}

} // namespace logos::integrity
