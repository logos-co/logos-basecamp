# Line/branch coverage for the C++ unit-test suite.
#
# Reuses tests/CMakeLists.txt verbatim — same targets, same ctest run as
# `nix build .#unit-tests` — but compiled with --coverage, then reported
# through gcovr. Build:
#
#   nix build .#coverage -L && open result/coverage.html
#
# Report-only by default (failUnderLine = 0). Raise the threshold as the
# test plan phases land, either here or from the caller in flake.nix, and the
# derivation starts failing the build when coverage regresses below it.
#
# Scope caveat: gcovr only sees files that were compiled into the test
# binaries. Sources no unit test links at all (PackageCoordinator,
# UIPluginManager, PluginLoader, MainContainer, MainUIBackend) produce no
# .gcno and therefore do NOT appear in the report as 0% — the percentage here
# is "coverage of the code under unit test", not of all of src/. Adding a
# source to tests/CMakeLists.txt is what pulls it into the denominator.
{ pkgs, src, logosPackageHeaders, failUnderLine ? 0, failUnderBranch ? 0 }:

let
  # gcov reader matching the stdenv compiler: clang emits gcov data only
  # llvm-cov of the same LLVM version can parse, gcc emits data for its own
  # gcov. Mismatching the two is the usual cause of "unknown gcov version".
  gcovExecutable =
    if pkgs.stdenv.cc.isClang
    then "${pkgs.llvmPackages.libllvm}/bin/llvm-cov gcov"
    else "${pkgs.stdenv.cc.cc}/bin/gcov";
in
pkgs.stdenv.mkDerivation {
  pname = "logos-basecamp-coverage";
  version = "0.0.0";

  inherit src;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsHook
    pkgs.gcovr
  ];
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtdeclarative   # Qt::Qml — InstallEnums.h includes <QtQml/qqml.h>
  ];

  dontUseCmakeConfigure = true;

  buildPhase = ''
    runHook preBuild
    # -fprofile-update=atomic keeps counters correct if a test ever spawns
    # threads; Debug already implies -O0 -g, which keeps line mapping exact.
    cmake -S tests -B build-cov -GNinja -DCMAKE_BUILD_TYPE=Debug \
      -DLOGOS_PACKAGE_HEADERS="${logosPackageHeaders}/include" \
      -DCMAKE_CXX_FLAGS="--coverage -fprofile-update=atomic" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage"
    cmake --build build-cov
    runHook postBuild
  '';

  # Running the suite is what writes the .gcda counters next to the .gcno
  # files in build-cov, so the report below depends on this phase.
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export QT_QPA_PLATFORM=offscreen
    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    ''}
    ctest --test-dir build-cov --output-on-failure
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out

    # --filter src/ keeps the report to production code (the tests' own
    # translation units and CMake's *_autogen moc stubs are excluded).
    gcovr \
      --root "$PWD" \
      --filter 'src/' \
      --exclude '.*_autogen.*' \
      --gcov-executable "${gcovExecutable}" \
      --exclude-unreachable-branches \
      --exclude-throw-branches \
      --print-summary \
      --html-title "logos-basecamp unit-test coverage" \
      --txt "$out/coverage.txt" \
      --html-details "$out/coverage.html" \
      --cobertura "$out/coverage.xml" \
      --json-summary "$out/summary.json" \
      --fail-under-line ${toString failUnderLine} \
      --fail-under-branch ${toString failUnderBranch}

    cat $out/coverage.txt
    runHook postInstall
  '';
}
