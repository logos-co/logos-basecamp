#include "MockBackendFixture.h"

#include "LogosBasecampPaths.h"


#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QtGlobal>

namespace {

constexpr const char* kResourcePath = ":/mock/mock-backend.json";
constexpr const char* kFixtureEnv   = "LOGOS_MOCK_FIXTURE";

bool    g_installed = false;
QString g_resolvedPath;

// Expand runtime paths into fixture strings.
//
// installDir/mainFilePath cannot be hardcoded: the bundle sits at a different
// absolute path every build, and UIPluginManager::hasBackendPlugin accepts
// .so/.dylib/.dll interchangeably, so a wrong suffix passes that gate and only
// fails later inside ui-host.
//
// The RESOLVED result is what gets written — other images have no idea where
// this application installed anything.
QJsonValue expandPlaceholders(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::String: {
        QString s = value.toString();
        if (!s.contains(QLatin1Char('{'))) return value;   // fast path
        s.replace(QStringLiteral("{PLUGINS_DIR}"),
                  LogosBasecampPaths::embeddedPluginsDirectory());
        s.replace(QStringLiteral("{MODULES_DIR}"),
                  LogosBasecampPaths::embeddedModulesDirectory());
        s.replace(QStringLiteral("{APP_DIR}"),
                  QCoreApplication::applicationDirPath());
        s.replace(QStringLiteral("{LIB_EXT}"),
#if defined(Q_OS_WIN)
                  QStringLiteral(".dll")
#elif defined(Q_OS_MAC)
                  QStringLiteral(".dylib")
#else
                  QStringLiteral(".so")
#endif
                  );
        return s;
    }
    case QJsonValue::Array: {
        QJsonArray out;
        for (const QJsonValue& v : value.toArray()) out.append(expandPlaceholders(v));
        return out;
    }
    case QJsonValue::Object: {
        QJsonObject out;
        const QJsonObject in = value.toObject();
        for (auto it = in.constBegin(); it != in.constEnd(); ++it)
            out.insert(it.key(), expandPlaceholders(it.value()));
        return out;
    }
    default:
        return value;
    }
}

// Where the resolved fixture is written.
//
// Under the user data dir, NOT beside the binary: a nix store path is
// read-only, and on macOS so is the inside of a signed .app bundle. This is
// also per-instance, so two Basecamps started with different --user-dir get
// their own resolved copies rather than racing on one file.
QString resolvedFixtureTarget()
{
    return QDir::cleanPath(LogosBasecampPaths::baseDirectory()
                           + QStringLiteral("/mock/mock-backend.resolved.json"));
}

} // namespace

namespace MockBackendFixture {

QString resolvedFixturePath() { return g_resolvedPath; }

void install()
{
    if (g_installed) return;
    g_installed = true;

    // An explicit override wins outright: it is already a path on disk, already
    // resolved by whoever wrote it, and already inherited by children. Do not
    // overwrite someone's hand-edited fixture with the baked-in one.
    const QByteArray existing = qgetenv(kFixtureEnv);
    if (!existing.isEmpty()) {
        g_resolvedPath = QString::fromUtf8(existing);
        qInfo().noquote() << "MockBackendFixture: using" << kFixtureEnv
                          << "override at" << g_resolvedPath;
        return;
    }

    QFile res(QString::fromLatin1(kResourcePath));
    if (!res.open(QIODevice::ReadOnly)) {
        qWarning() << "MockBackendFixture: no fixture compiled in at"
                   << kResourcePath
                   << "— the app will start with no modules and no canned calls.";
        return;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(res.readAll(), &err);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "MockBackendFixture: could not parse" << kResourcePath
                   << "-" << err.errorString() << "at offset" << err.offset;
        return;
    }

    const QJsonDocument resolved(expandPlaceholders(doc.object()).toObject());

    const QString target = resolvedFixtureTarget();
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        qWarning() << "MockBackendFixture: could not create" << target
                   << "— falling back to live modules, which are not there.";
        return;
    }

    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "MockBackendFixture: could not write" << target << "-"
                   << out.errorString();
        return;
    }
    out.write(resolved.toJson(QJsonDocument::Indented));
    out.close();

    qputenv(kFixtureEnv, target.toUtf8());
    g_resolvedPath = target;

    const int calls = resolved.object().value(QStringLiteral("calls")).toObject().size();
    const int modules = resolved.object().value(QStringLiteral("modules")).toArray().size();
    qInfo().noquote() << QStringLiteral(
        "MockBackendFixture: resolved fixture written to %1 "
        "(%2 module(s), %3 canned call entr(ies)) — %4 exported")
        .arg(target).arg(modules).arg(calls).arg(QString::fromLatin1(kFixtureEnv));
}

} // namespace MockBackendFixture
