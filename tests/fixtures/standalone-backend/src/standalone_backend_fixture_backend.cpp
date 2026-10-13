#include "standalone_backend_fixture_backend.h"

// Generated umbrella: LogosModules (behind modules()) from
// metadata.json#dependencies — typed wrappers + typed event accessors.
// (No dependencies here, but include it once you add some.)
// #include "logos_sdk.h"

int StandaloneBackendFixtureBackend::add(int a, int b)
{
    int result = a + b;
    // PROP from the .rep — QtRO pushes every setStatus to the QML replica.
    setStatus(QStringLiteral("%1 + %2 = %3").arg(a).arg(b).arg(result));
    return result;
}
