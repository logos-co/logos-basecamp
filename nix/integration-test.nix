# Integration tests for logos-basecamp.
# Launches the app with -platform offscreen, connects to the Qt Inspector,
# and runs UI tests (click buttons, verify text, etc.).
#
# Requires Node.js for the test runner and the Qt offscreen platform plugin.
{ pkgs, src, appPkg, logosQtMcp, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 120
# Combined PR-gate budget; elapsed goes to $out/elapsed-seconds, combined check in shutdown-test.nix.
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

  # Pre-seed fixture A (test_qml_only v0.1.1, spec §0.A) into <user-dir>/plugins/
  # so sidebar/dock tests have a real launcher app from the first boot.
  ${pkgs.nodejs}/bin/node --input-type=module -e "
    import { seedPlugin, FIXTURE_A } from '${src}/tests/fixtures/lgx.mjs';
    seedPlugin(process.env.LOGOS_USER_DIR, FIXTURE_A);
  "

  export QT_QPA_PLATFORM=offscreen
  export QT_FORCE_STDERR_LOGGING=1
  export QT_LOGGING_RULES="qt.*.debug=false;default.debug=true"

  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    export LD_LIBRARY_PATH="${pkgs.libGL}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ''}

  # Point test framework at the nix-built logos-qt-mcp package
  export LOGOS_QT_MCP="${logosQtMcp}"

  # Tee output into BASECAMP_APP_LOG for the G-ERR gate; exec keeps the framework-spawned PID.
  export BASECAMP_APP_LOG="$out/app.log"
  : > "$BASECAMP_APP_LOG"
  cat > app-with-log.sh <<'WRAPPER'
  #!${pkgs.bash}/bin/bash
  # Process substitution below is a bashism; keep the shebang explicitly bash.
  set -euo pipefail
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
