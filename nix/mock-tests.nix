# Correctness gate for the fixture-resolution half of the mock.
#
# Deliberately cheap: it builds mock/'s single source and its test, not
# Basecamp. That is what makes it affordable on every CI job, which is the
# point — mock/ compiles against Basecamp's own headers (LogosBasecampPaths)
# and encodes assumptions about where the bundle stages plugins, so a layout
# change can break it silently.
#
# The logos_core_* ABI conformance test moved to logos-liblogos with the code
# it exercises (tests/mock_core_abi_test.cpp there).
#
# Uses mock/tests/run-standalone.sh rather than CMake because logos-basecamp
# has no top-level CMakeLists.txt — app/ and src/ are separate CMake projects —
# so mock/CMakeLists.txt is reachable only from the mock app build.
{ pkgs, src }:

pkgs.stdenv.mkDerivation {
  pname = "logos-basecamp-mock-tests";
  version = "0.0.0";

  inherit src;

  nativeBuildInputs = [ pkgs.qt6.qtbase pkgs.pkg-config pkgs.bash ];
  buildInputs = [ pkgs.qt6.qtbase ];

  dontUseCmakeConfigure = true;
  dontConfigure = true;

  # Built and run inside checkPhase and never installed, so there is nothing
  # for wrapQtAppsHook to wrap.
  dontWrapQtApps = true;

  buildPhase = ''
    runHook preBuild
    # run-standalone.sh resolves siblings by walking up from mock/tests, so it
    # needs the repo laid out as <root>/mock/tests/.
    mkdir -p workspace/logos-basecamp
    cp -r . workspace/logos-basecamp/ 2>/dev/null || true
    chmod -R u+w workspace
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export QT_QPA_PLATFORM=offscreen
    export TMPDIR="$NIX_BUILD_TOP/mock-test-build"
    mkdir -p "$TMPDIR"
    # Pin the Qt tools to the same Qt as the headers. Qt puts rcc under
    # libexec/, so a PATH lookup finds nothing and — if another Qt is present —
    # silently picks its rcc, which fails with a version #error.
    export RCC="${pkgs.qt6.qtbase}/libexec/rcc"
    export MOC="${pkgs.qt6.qtbase}/libexec/moc"
    bash workspace/logos-basecamp/mock/tests/run-standalone.sh
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    echo "mock fixture tests passed" > $out/result.txt
    runHook postInstall
  '';

  meta.description = "Tests for logos-basecamp's mock fixture resolution";
}
