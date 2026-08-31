# Builds shell-preview: the Basecamp UI shell against a fixture, with no Logos
# code in the process.
#
# It links Qt and compiles against app/interfaces/ alone. There is deliberately
# NO logos input here -- not liblogos, not logos-protocol, not logos-qt-host,
# not logos-view-module-runtime. If this ever needs one, the UI/host boundary
# has leaked and the point of this derivation is gone.
#
# mainUIPlugin is a RUNTIME input only: the binary dlopens the same shipped
# main_ui the real host loads, unmodified. It is baked into the wrapper so
# `nix run .#shell-preview` works with no arguments.
{ pkgs, common, src, mainUIPlugin }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-shell-preview";
  version = common.version;

  inherit src;
  # NOT `inherit (common) meta` -- common.meta sets mainProgram = LogosBasecamp,
  # which is not the binary this derivation installs, and `nix run` obeys it.
  meta = common.meta // { mainProgram = "basecamp-shell-preview"; };

  nativeBuildInputs = common.nativeBuildInputs ++ [ pkgs.makeWrapper ];

  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtdeclarative
  ];

  configurePhase = ''
    runHook preConfigure

    export MACOSX_DEPLOYMENT_TARGET=12.0

    # $cmakeFlags FIRST, then Qt's host-TOOL paths -- see nix/main-ui.nix for
    # why this hand-rolled configurePhase needs both. Empty on native builds.
    cmake -S shell-preview -B build \
      $cmakeFlags \
      ${pkgs.lib.escapeShellArgs (pkgs.logosQtCrossCmakeFlags or [ ])} \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0

    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    cmake --build build
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp build/basecamp-shell-preview $out/bin/

    runHook postInstall
  '';

  # After Qt's setup hook has wrapped the binary, not before -- wrapping the
  # raw ELF ourselves would leave two nested wrappers.
  #
  # Defaults --shell to the main_ui this build was given, so the binary is
  # useful on its own. An explicit --shell still wins.
  postFixup = ''
    wrapProgram $out/bin/basecamp-shell-preview \
      --add-flags "--shell $(ls ${mainUIPlugin}/plugins/main_ui/main_ui.* | head -1)"
  '';
}
