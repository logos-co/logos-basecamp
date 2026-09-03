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

  # Stage the app-to-app intent fixtures into the throwaway user dir.
  #
  # Hand-written app directories — no nix build, no .lgx, no catalog, no
  # network. Two of them declare the SAME intent, which is the only way to
  # reach the ambiguous branch and the chooser: no two shipping apps provide
  # the same capability, and the real duplicate (wallet_ui) cannot be
  # co-installed.
  #
  # Without this the intent cases in ui-tests.mjs find no requester and skip
  # themselves, which reads as a pass.
  ${pkgs.bash}/bin/bash ${src}/tests/fixtures/intents/stage.sh "$LOGOS_USER_DIR"

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

  # A green suite is not evidence the UI is error-free: a QML TypeError fires
  # per-frame, prints to the app's stdout, and satisfies every assertion in this
  # file, because the assertions look at properties rather than at whether the
  # engine complained. That exact case shipped — a binding dereferencing an
  # absent field on every collapsed chooser row — and 74 tests passed over it.
  #
  # Reads $BASECAMP_APP_LOG rather than capturing the runner's own output: the
  # app-with-log.sh wrapper above already tees the app's stdout AND stderr there,
  # and it lands in $out, so a failing run can be read after the fact.
  #
  # SCRIPT errors only. Every one is a QML expression that threw, which means a
  # binding did not produce a value — always a bug, never environmental.
  # Deliberately NOT every qWarning: this run also emits QSettings and Fontconfig
  # complaints from the sandbox, and a pre-existing implicitWidth binding loop in
  # package_manager_ui's TableHeader. Failing on those would make the guard noise
  # and it would be switched off within a week.
  echo "Checking for QML script errors..."
  if grep -nE "(TypeError|ReferenceError|SyntaxError):|Unable to assign|is not a function" \
       "$BASECAMP_APP_LOG"; then
    echo ""
    echo "FAIL: the app logged QML script errors above."
    echo "The suite passed, which is the point: these do not fail assertions."
    exit 1
  fi

  echo "Integration tests passed in ''${elapsed}s (budget: ''${MCP_TEST_BUDGET_SECONDS}s for both PR-gate suites combined)"
  if [ "$elapsed" -gt "$MCP_TEST_BUDGET_SECONDS" ]; then
    echo "ERROR: ui-tests alone took ''${elapsed}s, over the ''${MCP_TEST_BUDGET_SECONDS}s combined budget" >&2
    exit 1
  fi
''
