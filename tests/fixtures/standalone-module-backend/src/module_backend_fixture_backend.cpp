#include "module_backend_fixture_backend.h"

#include <QDir>
#include <QFile>

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

void ModuleBackendFixtureBackend::onContextReady()
{
    writeMarker(instancePersistencePath(), "backend-loaded");
}

LogosShutdown ModuleBackendFixtureBackend::aboutToUnload()
{
    writeMarker(instancePersistencePath(), "backend-unloaded");
    return LogosShutdown::Synchronous;
}
