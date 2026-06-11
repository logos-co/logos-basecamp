#  Builds and runs basecamp's C++ unit tests (tests/apps_model_test.cpp).
#
#  Pure-model logic only — no IPC, no display, no PackageCoordinator. Mirrors
#  tests/sandbox/ in shape: a standalone QtTest project that links the real
#  basecamp sources under test so we exercise the same code the app does.
#  Add new tests by adding sources to tests/CMakeLists.txt and the AppsModel
#  source list there.
{ pkgs, src }:

pkgs.stdenv.mkDerivation {
  pname = "logos-basecamp-unit-tests";
  version = "0.0.0";

  inherit src;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsHook
  ];
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtdeclarative   # Qt::Qml — InstallStage.h includes <QtQml/qqml.h>
  ];

  dontUseCmakeConfigure = true;

  buildPhase = ''
    runHook preBuild
    cmake -S tests -B build-unit-tests -GNinja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-unit-tests
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export QT_QPA_PLATFORM=offscreen
    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
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
