# Smoke-tests the logos-basecamp binary.
# Launches the app with -platform offscreen and fails if:
#   - the app emits critical QML errors (engine failure, missing module, etc.)
#   - the app emits runtime QML errors (TypeError, ReferenceError, etc.)
#   - the app emits qCritical output
#   - the app crashes before the timeout (non-zero exit that isn't timeout's 124)
#   - the app exits too quickly with code 0 (indicates it didn't start properly)
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

  CRIT=$(grep -E "QQmlApplicationEngine failed|module.*is not installed|Cannot assign|failed to load component|Failed to load.*plugin|The shared library was not found|Failed to create plugins directory|qrc:.*error:|file:///.*error:|TypeError:|ReferenceError:|Cannot read property|Unable to assign \[undefined\]|Binding loop detected|CRITICAL:|qCritical" "$LOG" \
         | grep -vE "/plugins/[^/]+/" || true)
  if [ -n "$CRIT" ]; then
    echo "Critical errors detected:"
    echo "$CRIT"
    exit 1
  fi

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
