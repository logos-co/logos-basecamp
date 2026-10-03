# Runtime-process separation guard for logos-basecamp.
#
# The app spawns its runtime (logos_runtime) and calls it as its shell, so the
# token store never shares the app's process with the UI plugins it loads.
# tests/runtime-process-tests.mjs measures that in what each process maps:
# capability_module only in logos_runtime, each package module only in a
# logos_host_plain of its own, and all of them gone after SIGTERM on the app.
{ pkgs, src, appPkg, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 240 }:

pkgs.runCommand "logos-basecamp-runtime-process-test" {
  # nixpkgs' ps: macOS's /bin/ps is setuid, and a build user may not exec it.
  nativeBuildInputs = [ pkgs.coreutils pkgs.nodejs pkgs.ps ]
    ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.lsof ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
      pkgs.qt6.qtbase   # provides the offscreen platform plugin
      pkgs.libGL
      pkgs.libglvnd
    ];
} ''
  mkdir -p $out
  export LOGOS_USER_DIR="$out/app-data"
  mkdir -p "$LOGOS_USER_DIR"
  # Never take over the machine's basecamp:// handler from a test.
  export LOGOS_NO_SCHEME_REGISTER=1
  export QT_QPA_PLATFORM=offscreen
  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    export LD_LIBRARY_PATH="${pkgs.libGL}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ''}
  timeout ${toString timeoutSec} \
    ${pkgs.nodejs}/bin/node ${src}/tests/runtime-process-tests.mjs ${appBin}
''
