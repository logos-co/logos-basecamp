#pragma once

#include <QString>

// MockBackendFixture — point this Basecamp at a fixture instead of a runtime.
//
// Resolves the compiled-in fixture and exports LOGOS_MOCK_FIXTURE. Everything
// else follows from that variable: logos-protocol reports LogosMode::Mock while
// it is set, MockStore seeds itself from it, FixtureCoreRuntime reads the
// `modules` section, and child processes inherit it.
//
// It must be a FILE, not in-memory state: MockStore and the mode flag are
// per-image statics, so a plugin linking logos-protocol statically has its own
// copies that nothing outside its image can write to.

namespace MockBackendFixture {

// Resolve the compiled-in fixture, write it somewhere every image can read,
// and export LOGOS_MOCK_FIXTURE.
//
// MUST run before any LogosAPI is constructed: LogosAPI caches its clients
// under a key that includes the mode, so a client built beforehand keeps a
// Remote transport for the life of the process.
//
// Safe to call more than once; subsequent calls are no-ops.
void install();

// The resolved fixture path, or empty before install() has run. Exposed for
// tests and diagnostics.
QString resolvedFixturePath();

} // namespace MockBackendFixture
