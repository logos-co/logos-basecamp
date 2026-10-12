# Integration tests for a UI app, run against Basecamp in standalone mode.
#
# Launches app-standalone offscreen with the app (`--module`) and its
# dependencies (`--modules-dir`), and runs each Node.js test file with the
# logos-qt-mcp test framework, which drives the app through the QML inspector.
# Each file is a separate invocation (the framework's run() exits the process).
#
# Usage (logos-module-builder wires this up for ui_qml modules that pass
# `logosBasecamp`):
#   logos-basecamp.lib.${system}.mkPluginTest {
#     inherit pkgs;
#     installPkg = myModule.packages.${system}.install;   # modules/ + plugins/
#     modulesDir = depsDir;                               # optional
#     testFiles = [ ./tests/ui-tests.mjs ];
#   };
{ standaloneApp, logosQtMcp }:

{ pkgs, installPkg, modulesDir ? null, testFiles, timeoutSec ? 120
, name ? "plugin-integration-test"
, extraArgs ? ""   # appended to the launch command; shell-expanded at run time
, setup ? ""       # shell run before the tests (e.g. to stage a --qml-source dir)
}:

let
  launchScript = pkgs.writeShellScript "run-basecamp-standalone" ''
    exec ${standaloneApp}/bin/LogosBasecamp \
      --module ${installPkg} \
      ${pkgs.lib.optionalString (modulesDir != null) "--modules-dir ${modulesDir}"} \
      --user-dir "$BASECAMP_TEST_USER_DIR" ${extraArgs} "$@"
  '';

  # A fresh user dir per file, outside $out: one file's state must not reach
  # the next, and $out holds only the result.
  runOneTest = i: tf: ''
    echo "Running: ${builtins.baseNameOf (toString tf)}"
    export BASECAMP_TEST_USER_DIR="$TMPDIR/user-dir-${toString i}"
    mkdir -p "$BASECAMP_TEST_USER_DIR"
    timeout ${toString timeoutSec} \
      ${pkgs.nodejs}/bin/node ${tf} --ci ${launchScript} --verbose
    echo "${builtins.baseNameOf (toString tf)}" >> $out/passed
  '';
in
pkgs.runCommand name {
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

  export QT_QPA_PLATFORM=offscreen
  export QT_FORCE_STDERR_LOGGING=1
  export QT_LOGGING_RULES="qt.*.debug=false;default.debug=true"

  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    export LD_LIBRARY_PATH="${pkgs.libGL}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ''}

  export LOGOS_QT_MCP="${logosQtMcp}"

  ${setup}

  echo "Running integration tests: ${name} (${toString (builtins.length testFiles)} file(s), timeout: ${toString timeoutSec}s per file)..."

  ${pkgs.lib.concatStringsSep "\n" (pkgs.lib.imap0 runOneTest testFiles)}

  echo "All integration tests passed"
''
