# Builds the main UI shell as a Qt plugin: plugins/main_ui/main_ui.{so,dylib,dll}
#
# The shell talks to the host through IShellHost and compiles against Qt plus
# app/interfaces/ alone. logosDesignSystem is the only logos input, and it is
# QML-only. If this ever needs a logos input that carries CODE, something has
# leaked across the boundary -- check nix/symbol-gate.nix before adding it.
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
    # FindThreads probes for pthreads: "Qt6 could not be found because
    # dependency Threads could not be found". The next line carries Qt's
    # host-TOOL package paths. Both are empty on native builds.
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

    # plugins/main_ui/, not lib/: the layout app/window.cpp resolves and
    # nix/app.nix's PE import sweep walks.
    mkdir -p $out/plugins/main_ui

    _found=""
    for _ext in dylib dll so; do
      if [ -f "build/main_ui.$_ext" ]; then
        cp "build/main_ui.$_ext" "$out/plugins/main_ui/"
        _found="build/main_ui.$_ext"
        break
      fi
    done

    # Fail loudly: a silently missing plugin is an app that starts, finds no
    # shell, and qFatal()s far from this derivation.
    if [ -z "$_found" ]; then
      echo "error: no main_ui library was produced in build/"
      ls -la build/ || true
      exit 1
    fi
    echo "Installed $_found"

    runHook postInstall
  '';
}
