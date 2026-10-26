# Where apps download from, end to end: tests/download-source-tests.mjs.
# A fresh app and user dir per scenario, so onboarding runs each time; the
# App Manager and the Package Manager are checked together in each state.
#
# Requires Node.js, the Qt offscreen platform, and the MCP inspector.
{ pkgs, src, appPkg, logosQtMcp, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 600 }:

pkgs.runCommand "logos-basecamp-download-source-test" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.nodejs ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
      pkgs.qt6.qtbase
      pkgs.libGL
      pkgs.libglvnd
    ];
} ''
  mkdir -p $out
  export HOME="$TMPDIR/home"
  mkdir -p "$HOME"

  # Never take over the machine's basecamp:// handler from a test.
  export LOGOS_NO_SCHEME_REGISTER=1

  export QT_QPA_PLATFORM=offscreen
  export QT_FORCE_STDERR_LOGGING=1

  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    export LD_LIBRARY_PATH="${pkgs.libGL}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ''}

  export LOGOS_QT_MCP="${logosQtMcp}"

  set -o pipefail
  echo "Running logos-basecamp download source tests (timeout: ${toString timeoutSec}s)..."
  timeout ${toString timeoutSec} \
    ${pkgs.nodejs}/bin/node ${src}/tests/download-source-tests.mjs ${appBin} | tee $out/results.txt
''
