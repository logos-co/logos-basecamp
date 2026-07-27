# Smoke-tests the logos-basecamp binary.
# A pure boot heartbeat: launches the app with -platform offscreen and fails if:
#   - the app crashes before the timeout (non-zero exit that isn't timeout's 124)
#   - the app exits too quickly with code 0 (indicates it didn't start properly)
#
# Deliberately does NOT scan the log for error patterns — QML/plugin error
# assertions belong in integration-test (real UI walk with behavioural
# assertions) and unit-tests, not in a boot heartbeat. Log-regex was fragile
# (missed real bugs that never emitted the exact string, and tripped on any
# stderr line matching the pattern from unrelated code paths like graceful
# shutdown teardown noise).
#
# If the app crashes it exits immediately — the timeout is only ever waited out
# on the happy path (app stays alive and healthy).
#
# The LogosBasecamp launcher (bin/LogosBasecamp) bakes in the correct
# QT_PLUGIN_PATH and LD_LIBRARY_PATH at build time for dev builds.
# We only need to add the offscreen platform plugin and GL stubs on Linux.
{ pkgs, appPkg, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 5 }:

pkgs.runCommand "logos-basecamp-smoke-test" {
  nativeBuildInputs = [ pkgs.coreutils ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
      pkgs.qt6.qtbase   # provides the offscreen platform plugin
      pkgs.libGL
      pkgs.libglvnd
    ];
} ''

  mkdir -p $out
  export LOGOS_USER_DIR="$out/app-data"
  mkdir -p "$LOGOS_USER_DIR"

  export QT_QPA_PLATFORM=offscreen
  export QT_FORCE_STDERR_LOGGING=1
  export QT_LOGGING_RULES="qt.*.debug=false;default.debug=true"

  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    export LD_LIBRARY_PATH="${pkgs.libGL}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ''}

  LOG="$out/smoke-test.log"

  echo "Running logos-basecamp smoke test (timeout: ${toString timeoutSec}s)..."
  set +e
  START=$(date +%s)
  timeout ${toString timeoutSec} ${appBin} -platform offscreen > "$LOG" 2>&1
  CODE=$?
  END=$(date +%s)
  set -e

  cat "$LOG"

  # timeout returns 124 when it kills the process (expected — app runs an event loop)
  if [ "$CODE" -ne 0 ] && [ "$CODE" -ne 124 ]; then
    echo "App crashed with exit code $CODE"
    exit 1
  fi

  ELAPSED=$(( END - START ))
  if [ "$CODE" -eq 0 ] && [ "$ELAPSED" -lt 2 ]; then
    echo "App exited too quickly (''${ELAPSED}s) — likely failed to start"
    exit 1
  fi

  echo "Smoke test passed (exit code: $CODE, elapsed: ''${ELAPSED}s)"
''
