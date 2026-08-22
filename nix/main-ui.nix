# Builds the main UI shell as a Qt plugin: plugins/main_ui/main_ui.{so,dylib,dll}
#
# Deliberately rebuilt from the app derivation minus the runtime, NOT restored
# from the pre-fold revision. The old version staged a generated_code directory,
# ran logos-cpp-generator, copied module API headers out of
# logos-package-manager-module / logos-package-downloader-module, and passed six
# -DLOGOS_*_ROOT flags. The shell needs none of it any more: it talks to the host
# through IShellHost and compiles against Qt plus app/interfaces/ alone.
#
# If this derivation ever needs a logos input again, that is the signal that
# something has leaked back across the boundary — check nix/symbol-gate.nix
# before adding it.
{ pkgs, common, src, logosDesignSystem, distributed ? false }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-main-ui-plugin";
  version = common.version;

  inherit src;
  inherit (common) meta;

  nativeBuildInputs = common.nativeBuildInputs;

  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtdeclarative
    logosDesignSystem
  ];

  configurePhase = ''
    runHook preConfigure

    # Match the deployment target the Qt frameworks were built against.
    export MACOSX_DEPLOYMENT_TARGET=12.0

    # $cmakeFlags FIRST -- this hand-rolled configurePhase bypasses the cmake
    # setup hook, so without it -DCMAKE_SYSTEM_NAME=Windows is dropped and
    # FindThreads probes for pthreads, surfacing as "Qt6 could not be found
    # because dependency Threads could not be found". The second line carries
    # Qt's host-TOOL package paths. Both are empty on native builds.
    cmake -S src -B build \
      $cmakeFlags \
      ${pkgs.lib.escapeShellArgs (pkgs.logosQtCrossCmakeFlags or [ ])} \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
      -DLOGOS_DISTRIBUTED_BUILD=${if distributed then "ON" else "OFF"} \
      -DLOGOS_PORTABLE_BUILD=${if distributed then "ON" else "OFF"} \
      -DLogosDesignSystem_DIR=${logosDesignSystem}/lib/cmake/LogosDesignSystem

    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    cmake --build build
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    # Staged as plugins/main_ui/ rather than lib/, because that is the layout
    # app/window.cpp resolves and the one nix/app.nix's PE import sweep walks.
    mkdir -p $out/plugins/main_ui

    _found=""
    for _ext in dylib dll so; do
      if [ -f "build/main_ui.$_ext" ]; then
        cp "build/main_ui.$_ext" "$out/plugins/main_ui/"
        _found="build/main_ui.$_ext"
        break
      fi
    done

    # Fail loudly. A plugin that silently fails to install is an app that starts,
    # finds no shell, and qFatal()s at a point far from this derivation.
    if [ -z "$_found" ]; then
      echo "error: no main_ui library was produced in build/"
      ls -la build/ || true
      exit 1
    fi
    echo "Installed $_found"

    runHook postInstall
  '';
}
