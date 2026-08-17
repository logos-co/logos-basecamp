# Common build configuration shared across all packages
{ pkgs, logosSdk, logosProtocolPkg, logosQtSdk, logosModule, logosLiblogos }:

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
    pkgs.zlib
    pkgs.icu
  ]
  # abseil-cpp does not cross-compile to mingw: absl/base/internal/
  # thread_identity.cc includes <pthread.h>, which mingw-w64 with mcfgthread
  # does not ship. Like krb5 below it is dead weight here -- "absl" appears
  # nowhere in the sources or CMake, only in this file and the README.
  ++ pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows) pkgs.abseil-cpp
  # krb5 does not cross-evaluate to mingw: it carries a host-platform `bash` in
  # its own buildInputs, so the splice fails with "Refusing to evaluate package
  # 'bash-5.3p9' ... not available on the requested hostPlatform" -- an error
  # naming bash, several levels away from the actual cause.
  #
  # Guarded rather than deleted: nothing in Basecamp links Kerberos directly
  # (it arrives as an optional Qt network transitive), but it is left in place
  # on Unix so this stays a Windows change and not a behavioural one.
  ++ pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows) pkgs.krb5;
  
  # Common CMake flags
  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_PROTOCOL_ROOT=${logosProtocolPkg}"
    "-DLOGOS_QT_SDK_ROOT=${logosQtSdk}"
    "-DLOGOS_MODULE_ROOT=${logosModule}"
    "-DLOGOS_LIBLOGOS_ROOT=${logosLiblogos}"
  ]
  # Qt splits its host TOOLS (repc, moc, qmltyperegistrar, qsb) into separate
  # packages that must run on the BUILD machine; -DQT_HOST_PATH=<qtbase> cannot
  # reach them. Empty on native builds. The symptom when absent is actively
  # misleading: CMake reports 'Failed to find required Qt component
  # "RemoteObjects"', but the TARGET Qt6RemoteObjects is present -- it is
  # Qt6RemoteObjectsTools, the host-side package, that is missing.
  ++ (pkgs.logosQtCrossCmakeFlags or [ ]);
  
  # Environment variables
  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_PROTOCOL_ROOT = "${logosProtocolPkg}";
    LOGOS_QT_SDK_ROOT = "${logosQtSdk}";
    LOGOS_MODULE_ROOT = "${logosModule}";
    LOGOS_LIBLOGOS_ROOT = "${logosLiblogos}";
  };
  
  # Metadata
  meta = with pkgs.lib; {
    description = "Logos Basecamp - Qt application with UI plugins";
    platforms = platforms.unix ++ platforms.windows;
    mainProgram = "LogosBasecamp";
  };
}
