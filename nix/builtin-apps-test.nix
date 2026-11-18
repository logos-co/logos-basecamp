# Basecamp's own apps run only from its bundle (app/utils/BuiltInApps.h), end
# to end against the real core and package manager. tests/builtin-apps-tests.mjs
# plants copies of Package Manager in a fresh user dir and drives the app, once
# per scenario:
#
#   moved  copies planted before start and while it runs are moved aside, and
#          the backend that runs is Basecamp's own;
#   stuck  copies that cannot be moved stay, and Package Manager is refused.
#
# The log gates below then check what Basecamp told the user, which the test
# itself cannot read.
{ pkgs, src, appPkg, logosQtMcp, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 300 }:

pkgs.runCommand "logos-basecamp-builtin-apps-test" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.nodejs ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
      pkgs.qt6.qtbase   # provides the offscreen platform plugin
      pkgs.libGL
      pkgs.libglvnd
    ];
} ''
  mkdir -p $out

  # As nix/host-services-test.nix: no basecamp:// registration from a test.
  export LOGOS_NO_SCHEME_REGISTER=1
  export QT_QPA_PLATFORM=offscreen
  export QT_FORCE_STDERR_LOGGING=1
  export QT_LOGGING_RULES="qt.*.debug=false;default.debug=true"
  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    export LD_LIBRARY_PATH="${pkgs.libGL}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ''}
  export LOGOS_QT_MCP="${logosQtMcp}"
  export BUILTIN_APP_ROOT="${appPkg}"

  scenario() {
    export BUILTIN_SCENARIO="$1" LOGOS_USER_DIR="$out/$1"
    mkdir -p "$LOGOS_USER_DIR"
    set +e
    timeout ${toString timeoutSec} \
      ${pkgs.nodejs}/bin/node ${src}/tests/builtin-apps-tests.mjs --ci ${appBin} --verbose \
      > "$out/$1.log" 2>&1
    code=$?
    set -e
    cat "$out/$1.log"
    if [ "$code" -ne 0 ]; then
      echo "FAIL: scenario $1 exited $code (124: timed out after ${toString timeoutSec}s)"
      exit 1
    fi
  }

  # Each line must be in the scenario's session log as many times as given:
  # Basecamp's own log, one write per line, where the test's --verbose copy
  # can splice the app's two streams mid-line.
  said() {
    found=$(cat "$out/$1"/logs/basecamp_*.log | grep -cF -- "$3" || true)
    if [ "''${found:-0}" -lt "$2" ]; then
      echo "FAIL: scenario $1 said \"$3\" ''${found:-0} times, not $2"
      exit 1
    fi
  }

  scenario moved
  said moved 2 "package_manager_ui comes with Basecamp, so only Basecamp's own copy runs. The copy that was in"
  said moved 2 "package_manager_ui_backend comes with Basecamp, so only Basecamp's own copy runs. The copy that was in"
  said moved 1 "Basecamp stopped what was running under that name"

  scenario stuck
  said stuck 1 "package_manager_ui comes with Basecamp, so only Basecamp's own copy runs. The copy in"
  said stuck 1 "package_manager_ui_backend comes with Basecamp, so only Basecamp's own copy runs. The copy in"
  said stuck 1 'Refusing to load "package_manager_ui"'

  echo "Built-in apps test passed (moved, stuck)"
''
