# Builds and runs basecamp's QML component tests (Qt Quick Test).
pkgs.stdenv.mkDerivation {
  pname = "logos-basecamp-qml-tests";
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
    pkgs.qt6.qtdeclarative   # QQuickTest / QML engine
  ];

  dontUseCmakeConfigure = true;

  buildPhase = ''
    runHook preBuild
    cmake -S tests/qml -B build-qml-tests -GNinja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-qml-tests
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export QT_QPA_PLATFORM=offscreen
    export QML2_IMPORT_PATH="${pkgs.qt6.qtdeclarative}/${pkgs.qt6.qtbase.qtQmlPrefix}''${QML2_IMPORT_PATH:+:$QML2_IMPORT_PATH}"
    export QT_QML_IMPORT_PATH="$QML2_IMPORT_PATH"
    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    ''}
    ctest --test-dir build-qml-tests --output-on-failure
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp build-qml-tests/qml_tests $out/ 2>/dev/null || true
    echo "basecamp qml tests passed" > $out/result.txt
    runHook postInstall
  '';
}
