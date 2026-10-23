#pragma once

#include <QString>

// Whether this user dir still has first-run onboarding ahead of it, where the
// user picks how apps are downloaded before the storage node can start.
//
// Kept under LogosBasecampPaths::baseDirectory(), not QSettings: --user-dir
// then starts a fresh onboarding, and dev and portable builds keep their own.
namespace OnboardingState {

// False once completed. Also false, and recorded as completed, for a user dir
// Basecamp has used before: an existing user keeps the download source they
// already have. module_data/package_downloader/ is no sign of that: the host
// creates it on every load.
bool required(const QString& baseDir);

void markCompleted(const QString& baseDir);

} // namespace OnboardingState
