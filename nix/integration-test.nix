# Integration tests for logos-basecamp.
# Launches the app with -platform offscreen, connects to the Qt Inspector,
# and runs UI tests (click buttons, verify text, etc.).
#
# Requires Node.js for the test runner and the Qt offscreen platform plugin.
{ pkgs, src, appPkg, logosQtMcp, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 120
# PR-gate wall-clock budget for BOTH suites (ui-tests + shutdown-tests)
# combined. This derivation records its elapsed time in $out/elapsed-seconds
# and can only fail early when its own run alone busts the budget; the
# combined check lives in nix/shutdown-test.nix, which reads that file.
, budgetSec ? 600 }:

pkgs.runCommand "logos-basecamp-integration-test" {
  MCP_TEST_BUDGET_SECONDS = toString budgetSec;
  nativeBuildInputs = [ pkgs.coreutils pkgs.nodejs ]
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

  # Point test framework at the nix-built logos-qt-mcp package
  export LOGOS_QT_MCP="${logosQtMcp}"

  # G-ERR wiring (tests/fixtures/harness.mjs): the harness's file-mode QML
  # error gate scans the file named by BASECAMP_APP_LOG, taking a byte-offset
  # baseline at each test's start. The framework (logos-qt-mcp — external
  # flake input, not editable here) launches the app itself, so interpose a
  # wrapper "binary" that tees the app's stdout/stderr into that file while
  # passing both streams through unchanged (exec keeps the app on the PID the
  # framework spawned, so its kill still lands). QT_FORCE_STDERR_LOGGING
  # (above) keeps QML engine errors on stderr even though it is now a pipe.
  export BASECAMP_APP_LOG="$out/app.log"
  : > "$BASECAMP_APP_LOG"
  cat > app-with-log.sh <<'WRAPPER'
  #!${pkgs.runtimeShell}
  exec ${appBin} "$@" \
    > >(tee -a "$BASECAMP_APP_LOG") \
    2> >(tee -a "$BASECAMP_APP_LOG" >&2)
  WRAPPER
  chmod +x app-with-log.sh

  echo "Running logos-basecamp integration tests (timeout: ${toString timeoutSec}s)..."

  start=$(date +%s)
  timeout ${toString timeoutSec} \
    ${pkgs.nodejs}/bin/node ${src}/tests/ui-tests.mjs --ci "$PWD/app-with-log.sh" --verbose
  elapsed=$(( $(date +%s) - start ))
  echo "$elapsed" > $out/elapsed-seconds

  echo "Integration tests passed in ''${elapsed}s (budget: ''${MCP_TEST_BUDGET_SECONDS}s for both PR-gate suites combined)"
  if [ "$elapsed" -gt "$MCP_TEST_BUDGET_SECONDS" ]; then
    echo "ERROR: ui-tests alone took ''${elapsed}s, over the ''${MCP_TEST_BUDGET_SECONDS}s combined budget" >&2
    exit 1
  fi
''
