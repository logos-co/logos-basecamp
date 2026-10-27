#include "module_backend_fixture_backend.h"

#include <QDir>
#include <QFile>
#include <QTimer>
#include <cstdlib>

namespace {
void writeMarker(const std::string& dir, const char* name)
{
    QFile f(QDir(QString::fromStdString(dir)).filePath(QString::fromLatin1(name)));
    if (f.open(QIODevice::WriteOnly)) f.write(name);
}
}

int ModuleBackendFixtureBackend::add(int a, int b)
{
    const int result = a + b;
    setStatus(QStringLiteral("%1 + %2 = %3").arg(a).arg(b).arg(result));
    return result;
}

void ModuleBackendFixtureBackend::crash()
{
    std::abort();
}

void ModuleBackendFixtureBackend::onContextReady()
{
    writeMarker(instancePersistencePath(), "backend-loaded");
    // For trying Basecamp's crash handling by hand; the tests never set it.
    //   MODULE_BACKEND_FIXTURE_CRASH=start      crash while the app opens
    //   MODULE_BACKEND_FIXTURE_CRASH=after:<s>  crash <s> seconds after that
    const QString crash = qEnvironmentVariable("MODULE_BACKEND_FIXTURE_CRASH");
    if (crash == QStringLiteral("start")) std::abort();
    if (crash.startsWith(QStringLiteral("after:")))
        QTimer::singleShot(crash.mid(6).toInt() * 1000, [] { std::abort(); });
}

LogosShutdown ModuleBackendFixtureBackend::aboutToUnload()
{
    writeMarker(instancePersistencePath(), "backend-unloaded");
    return LogosShutdown::Synchronous;
}
