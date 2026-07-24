# Shutdown tests for logos-basecamp.
# Spawns a fresh app instance per test and verifies each quit gesture
# (SIGTERM, SIGINT, Ctrl+Q on Linux, ⌘Q on macOS) terminates the process
# cleanly via the orderly teardown in app/main.cpp.
#
# Requires Node.js, the Qt offscreen platform, and the MCP inspector.
{ pkgs, src, appPkg, logosQtMcp, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 180 }:

pkgs.runCommand "logos-basecamp-shutdown-test" {
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

  timeout ${toString timeoutSec} \
    ${pkgs.nodejs}/bin/node ${src}/tests/shutdown-tests.mjs ${appBin}

  echo "Shutdown tests passed"
''
