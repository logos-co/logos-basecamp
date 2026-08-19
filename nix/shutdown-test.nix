# Shutdown tests for logos-basecamp.
# Spawns a fresh app instance per test and verifies each quit gesture
# (SIGTERM, SIGINT, Ctrl+Q on Linux, ⌘Q on macOS) terminates the process
# cleanly via the orderly teardown in app/main.cpp.
#
# Requires Node.js, the Qt offscreen platform, and the MCP inspector.
{ pkgs, src, appPkg, logosQtMcp, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 180
# PR-gate wall-clock budget for BOTH suites (ui-tests + shutdown-tests)
# combined. `uiTestRun` is the platform's integration-test derivation; its
# $out/elapsed-seconds is added to this suite's elapsed time and the build
# fails when the combined total exceeds the budget.
, budgetSec ? 600, uiTestRun ? null }:

pkgs.runCommand "logos-basecamp-shutdown-test" {
  MCP_TEST_BUDGET_SECONDS = toString budgetSec;
  nativeBuildInputs = [ pkgs.coreutils pkgs.nodejs ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
      pkgs.qt6.qtbase
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

  export LOGOS_QT_MCP="${logosQtMcp}"

  echo "Running logos-basecamp shutdown tests (timeout: ${toString timeoutSec}s)..."

  start=$(date +%s)
  timeout ${toString timeoutSec} \
    ${pkgs.nodejs}/bin/node ${src}/tests/shutdown-tests.mjs ${appBin}
  elapsed=$(( $(date +%s) - start ))
  echo "$elapsed" > $out/elapsed-seconds

  combined=$elapsed
  ${pkgs.lib.optionalString (uiTestRun != null) ''
    prior=$(cat ${uiTestRun}/elapsed-seconds)
    combined=$(( combined + prior ))
    echo "Shutdown tests took ''${elapsed}s; combined with the integration test (''${prior}s): ''${combined}s"
  ''}
  echo "Shutdown tests passed (combined suite time: ''${combined}s, budget: ''${MCP_TEST_BUDGET_SECONDS}s)"
  if [ "$combined" -gt "$MCP_TEST_BUDGET_SECONDS" ]; then
    echo "ERROR: the PR-gate suites took ''${combined}s combined, over the ''${MCP_TEST_BUDGET_SECONDS}s budget" >&2
    exit 1
  fi
''
