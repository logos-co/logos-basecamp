#pragma once

#include <QString>

#include <string>

// package_downloader's repository calls. A change fetches the repository's
// index, which the downloader gives 60 s.
constexpr int kRepositoryReadMs = 30 * 1000;
constexpr int kRepositoryChangeMs = 90 * 1000;
// A refresh fetches every enabled repository's index, all at once.
constexpr int kCatalogRefreshMs = 5 * 60 * 1000;

// What a failed call to `module` tells the user: a timeout as a timeout, else
// the module's own words. `code` and `message` are a logos::CallError's.
QString moduleCallFailure(const QString& module, const std::string& code,
                          const std::string& message, int timeoutMs);

// The same for a change: one that timed out may still be made, as the module
// carries on after we stop waiting.
QString moduleChangeFailure(const QString& module, const std::string& code,
                            const std::string& message, int timeoutMs);
