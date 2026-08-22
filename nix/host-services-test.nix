# Host-services grant guard for logos-basecamp.
#
# Launches the app with -platform offscreen, opens the Package Manager section
# (which spawns ui-host under the NON-"core" identity "package_manager_ui") and
# asserts that a capability-gated call chain actually COMPLETED — see
# tests/host-services-assert.mjs for why pmui.BackendStore.repositoryCount is
# the witness and why the neighbouring catalog numbers are not.
#
# ── Why this is a check of its own ──────────────────────────────────────────
#
# The same assertion is also registered inside tests/ui-tests.mjs, so
# `integration-test` carries it too. This derivation exists because the failure
# it guards is a PIN failure (basecamp taking a logos-liblogos /
# default-module-loader that predates the grant), and a pin failure deserves a
# gate whose name says what broke rather than one buried in a 17-test UI walk.
# It also adds a second gate the UI harness cannot: a scan of the app's own
# stderr for refusals.
#
# ── The two gates, and why both ─────────────────────────────────────────────
#
#   1. POSITIVE, in-process: a privileged operation must SUCCEED. Cannot pass
#      vacuously — if PMUI never loads, the spec fails rather than skips.
#   2. NEGATIVE, post-run log scan: zero "rejecting unauthorized call" and zero
#      "was not granted the <service> host service". This is an ABSENCE
#      assertion and would pass vacuously on its own (an app that makes no
#      gated calls at all refuses nothing) — it is only meaningful because
#      gate 1 has already established that the calls were made.
#
# Requires Node.js for the test runner and the Qt offscreen platform plugin.
{ pkgs, src, appPkg, logosQtMcp, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 240 }:

pkgs.runCommand "logos-basecamp-host-services-test" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.nodejs ]
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

  LOG="$out/host-services-test.log"

  echo "Running logos-basecamp host-services grant guard (timeout: ${toString timeoutSec}s)..."

  # --verbose so the app's (and, through it, ui-host's and each module's) stderr
  # lands in $LOG for gate 2 below.
  #
  # Exit code captured explicitly rather than relying on set -e: `timeout`
  # reports 124 on its own kill, and a silently-swallowed 124 would leave gate 2
  # scanning a truncated log and reporting green.
  set +e
  timeout ${toString timeoutSec} \
    ${pkgs.nodejs}/bin/node ${src}/tests/host-services-tests.mjs --ci ${appBin} --verbose \
    > "$LOG" 2>&1
  RUN_CODE=$?
  set -e

  cat "$LOG"

  # ── Gate 2: the app must not have refused a single gated call ──────────────
  #
  # Counted BEFORE gate 1's verdict is applied, and always printed, so a red run
  # is self-diagnosing: refused>0 means the grant is genuinely broken, whereas
  # refused=0 alongside a gate-1 failure points at the environment (PMUI never
  # loaded, ui-host never started) rather than at the grant.
  #
  # `grep -c` exits 1 on zero matches, so each count is taken with `|| true`;
  # the `:-0` guards the (log-missing) case where grep prints nothing at all.
  REFUSED=$(grep -c "ModuleProxy: rejecting unauthorized call" "$LOG" || true)
  UNGRANTED=$(grep -c "was not granted the token_registry host service" "$LOG" || true)
  UNGRANTED_DELIVERY=$(grep -c "was not granted the token_delivery host service" "$LOG" || true)
  REFUSED=''${REFUSED:-0}
  UNGRANTED=''${UNGRANTED:-0}
  UNGRANTED_DELIVERY=''${UNGRANTED_DELIVERY:-0}

  echo ""
  echo "Gate 2 counts: refused=$REFUSED ungranted(token_registry)=$UNGRANTED ungranted(token_delivery)=$UNGRANTED_DELIVERY"

  if [ "$RUN_CODE" -eq 124 ]; then
    echo ""
    echo "FAIL: the guard did not finish within ${toString timeoutSec}s (timeout killed it)."
    exit 1
  fi
  if [ "$RUN_CODE" -ne 0 ]; then
    echo ""
    echo "FAIL: gate 1 (a privileged operation must succeed) failed with exit $RUN_CODE."
    echo "      See the gate 2 counts above: a non-zero refused/ungranted count means the"
    echo "      host-services grant did not reach capability_module; all-zero counts point"
    echo "      at the run never getting far enough to make a gated call."
    exit 1
  fi

  if [ "$REFUSED" -ne 0 ] || [ "$UNGRANTED" -ne 0 ] || [ "$UNGRANTED_DELIVERY" -ne 0 ]; then
    echo ""
    echo "FAIL: gate 2 — the run contains refused capability-gated calls."
    echo "      capability_module fails CLOSED when it is missing the token_registry /"
    echo "      token_delivery host services, and every cross-identity call then comes"
    echo "      back as the unauthorized sentinel. Offending lines:"
    grep -n "ModuleProxy: rejecting unauthorized call\|was not granted the token_" "$LOG" | head -40
    exit 1
  fi

  echo ""
  echo "Host-services grant guard passed (gate 1: privileged operation succeeded; gate 2: 0 refusals)"
''
