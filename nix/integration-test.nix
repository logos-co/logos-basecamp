# Integration tests for logos-basecamp.
# Launches the app with -platform offscreen, connects to the Qt Inspector,
# and runs UI tests (click buttons, verify text, etc.).
#
# Requires Node.js for the test runner and the Qt offscreen platform plugin.
{ pkgs, src, appPkg, logosQtMcp, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 120 }:

pkgs.runCommand "logos-basecamp-integration-test" {
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
  # Pin the update check at a file:// stub rather than disabling it.
  #
  # Hermeticity comes from the stub, not from the kill switch -- no socket is
  # opened either way. Disabling would instead unregister the two update tests
  # in ui-tests.mjs (they self-skip on LOGOS_DISABLE_UPDATE_CHECK), so this --
  # the ONLY automated runner of that suite -- would print a green skip forever
  # and the feature's UI coverage would never actually execute in CI.
  #
  # Without an override the check would be eligible on a release/** branch
  # (VERSION present) and hit api.github.com, which succeeds on a sandbox=false
  # host and DNS-fails on Linux CI: the same derivation behaving two ways.
  export LOGOS_UPDATE_CHECK_URL="file://${src}/tests/fixtures/update-release-stub.json"
  export LOGOS_UPDATE_CURRENT_VERSION=0.0.1
  export QT_FORCE_STDERR_LOGGING=1
  export QT_LOGGING_RULES="qt.*.debug=false;default.debug=true"

  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    export LD_LIBRARY_PATH="${pkgs.libGL}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ''}

  # Point test framework at the nix-built logos-qt-mcp package
  export LOGOS_QT_MCP="${logosQtMcp}"

  echo "Running logos-basecamp integration tests (timeout: ${toString timeoutSec}s)..."

  timeout ${toString timeoutSec} \
    ${pkgs.nodejs}/bin/node ${src}/tests/ui-tests.mjs --ci ${appBin} --verbose

  echo "Integration tests passed"
''
