{ pkgs, src, logosPackageHeaders, logosViewModuleRuntimeSrc
# The BUILT runtime, for the channel SurfaceTransport library (RFC-7). Null
# builds the suite without the channel backend and its tests.
, logosViewModuleRuntime ? null }:

let
  # qtwayland with the compositor-side wl_shm guard (RFC-4 rev 2 H1), the
  # same build the app links; the raw-client test exercises the guard.
  qtwayland = import ./qtwayland.nix { inherit pkgs; };
in
pkgs.stdenv.mkDerivation {
  pname = "logos-basecamp-unit-tests";
  version = "0.0.0";

  inherit src;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsHook
  ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
    pkgs.wayland-scanner     # view_surface_test: xdg-shell code for its raw client
  ];
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtdeclarative   # Qt::Qml — InstallStage.h includes <QtQml/qqml.h>
    # log_sink_test / logging_config_test declare these as `// libdeps:`.
    pkgs.spdlog
    pkgs.yaml-cpp
  ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
    # view_surface_test: the nested compositor (Qt6::WaylandCompositor) and
    # the wayland QPA plugin its fixture client connects with; libwayland
    # and the xdg-shell XML for the raw client (RFC-4 rev 2 §Tests).
    qtwayland
    pkgs.wayland
    pkgs.wayland-protocols
  ];

  dontUseCmakeConfigure = true;

  buildPhase = ''
    runHook preBuild
    cmake -S tests -B build-unit-tests -GNinja -DCMAKE_BUILD_TYPE=Debug \
      -DLOGOS_PACKAGE_HEADERS="${logosPackageHeaders}/include" \
      -DLOGOS_VIEW_MODULE_RUNTIME_ROOT="${logosViewModuleRuntimeSrc}" \
      ${pkgs.lib.optionalString (logosViewModuleRuntime != null)
        "-DLOGOS_VIEW_SURFACE_ROOT=${logosViewModuleRuntime}"}
    cmake --build build-unit-tests
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export QT_QPA_PLATFORM=offscreen
    # view_surface_test spawns a Qt wayland client; both it and the
    # compositor side want a writable HOME and runtime dir.
    export HOME="$TMPDIR/home"; mkdir -p "$HOME"
    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}:${qtwayland}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    ''}
    ctest --test-dir build-unit-tests --output-on-failure
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp build-unit-tests/apps_model_test $out/ 2>/dev/null || true
    echo "basecamp unit tests passed" > $out/result.txt
    runHook postInstall
  '';
}
