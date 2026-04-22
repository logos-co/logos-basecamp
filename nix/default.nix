# Common build configuration shared across all packages
{ pkgs, logosSdk, logosModule, logosLiblogos }:

{
  pname = "logos-basecamp";
  # VERSION is only present on release branches; dev branches use a placeholder.
  version = if builtins.pathExists ../VERSION
    then pkgs.lib.removeSuffix "\n" (builtins.readFile ../VERSION)
    else "0.0.0-dev";
  
  # Common native build inputs
  nativeBuildInputs = [ 
    pkgs.cmake 
    pkgs.ninja 
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsHook
  ];
  
  # Common runtime dependencies
  buildInputs = [ 
    pkgs.qt6.qtbase 
    pkgs.qt6.qtremoteobjects
    pkgs.qt6.qtdeclarative
    pkgs.zstd
    pkgs.krb5
    pkgs.abseil-cpp
    pkgs.zlib
    pkgs.icu
  ];
  
  # Common CMake flags
  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_MODULE_ROOT=${logosModule}"
    "-DLOGOS_LIBLOGOS_ROOT=${logosLiblogos}"
  ];
  
  # Environment variables
  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_MODULE_ROOT = "${logosModule}";
    LOGOS_LIBLOGOS_ROOT = "${logosLiblogos}";
  };
  
  # Metadata
  meta = with pkgs.lib; {
    description = "Logos App - Qt application with UI plugins";
    platforms = platforms.unix;
  };
}
