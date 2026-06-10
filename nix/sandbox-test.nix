# Builds and runs the ui_qml sandbox-escape regression test (tests/sandbox).
#
# This is a focused C++ unit test (QtTest) — not a full app launch. It builds a
# real malicious Qt QML plugin and asserts that the production sandbox config
# (src/restricted/QmlSandbox.cpp, the code PluginLoader::loadQmlView runs) refuses
# to load it, while a legitimate pure-QML module still loads. See
# tests/sandbox/tst_qml_sandbox.cpp for the full description (F-008).
{ pkgs, src }:

pkgs.stdenv.mkDerivation {
  pname = "logos-basecamp-sandbox-test";
  version = "0.0.0";

  inherit src;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsHook   # makes QtQml/QtQuick plugins discoverable at runtime
  ];
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtdeclarative
  ];

  dontUseCmakeConfigure = true;

  buildPhase = ''
    runHook preBuild
    cmake -S tests/sandbox -B build-sandbox-test -GNinja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-sandbox-test
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export QT_QPA_PLATFORM=offscreen
    # Stage the malicious plugin somewhere dlopen()-able. $TMPDIR inside the nix
    # builder is exec-capable; pin the test's scratch base to it explicitly.
    export SANDBOX_TEST_TMPDIR="$TMPDIR"
    # The test runs pre-install (before wrapQtAppsHook), so point the engine at
    # Qt's QML modules (QtQml's builtins) explicitly.
    export QML2_IMPORT_PATH="${pkgs.qt6.qtdeclarative}/${pkgs.qt6.qtbase.qtQmlPrefix}"
    export QML_IMPORT_PATH="$QML2_IMPORT_PATH"
    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    ''}
    ctest --test-dir build-sandbox-test --output-on-failure
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp build-sandbox-test/tst_qml_sandbox $out/ 2>/dev/null || true
    echo "ui_qml sandbox-escape regression test passed" > $out/result.txt
    runHook postInstall
  '';
}
