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
