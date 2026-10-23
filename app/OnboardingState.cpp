#include "OnboardingState.h"

#include <QDir>
#include <QFile>
#include <QSettings>

namespace {

const char* const kCompletedKey = "onboarding/completed";

QString statePath(const QString& baseDir)
{
    return QDir(baseDir).filePath(QStringLiteral("onboarding.ini"));
}

bool hasEntries(const QString& path)
{
    return !QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
}

// A previous session's log (this one has written its own by now), an installed
// app or module, or a saved downloader config.
bool usedBefore(const QString& baseDir)
{
    const QDir base(baseDir);

    if (QDir(base.filePath(QStringLiteral("logs")))
            .entryList({QStringLiteral("basecamp_*.log")}, QDir::Files).size() > 1)
        return true;

    if (hasEntries(base.filePath(QStringLiteral("modules")))
        || hasEntries(base.filePath(QStringLiteral("plugins"))))
        return true;

    const QDir downloader(base.filePath(QStringLiteral("module_data/package_downloader")));
    for (const QString& instance : downloader.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QFile::exists(downloader.filePath(instance + QStringLiteral("/repositories.json"))))
            return true;
    }

    return false;
}

} // namespace

namespace OnboardingState {

bool required(const QString& baseDir)
{
    if (QSettings(statePath(baseDir), QSettings::IniFormat).value(kCompletedKey).toBool())
        return false;

    if (usedBefore(baseDir)) {
        markCompleted(baseDir);
        return false;
    }

    return true;
}

void markCompleted(const QString& baseDir)
{
    // QSettings does not create missing parent directories.
    QDir().mkpath(baseDir);
    QSettings settings(statePath(baseDir), QSettings::IniFormat);
    settings.setValue(kCompletedKey, true);
    settings.sync();
}

} // namespace OnboardingState
