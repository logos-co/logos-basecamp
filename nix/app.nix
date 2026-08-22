# Builds the logos-basecamp standalone application.
{ pkgs, common, src, logosModule, logosLiblogos, logosSdk, logosSdkBuild ? logosSdk, logosProtocolPkg, logosQtHost, logosQtSdk, logosDesignSystem, logosViewModuleRuntime, logosPackageManagerModule, logosPackageDownloaderModule, logosPackageHeaders, buildInfo, logosQtMcp ? null, mainUIPlugin ? null, installedModules ? [], portable ? false, enableInspector ? true }:

let
  # webkitgtk became ABI-versioned; pick the newest available while staying
  # compatible with older nixpkgs where the unversioned attribute still exists.
  webkitgtk = pkgs.webkitgtk_4_1 or pkgs.webkitgtk_4_0 or pkgs.webkitgtk;

  buildInfoHeader = import ./build-info.nix { inherit pkgs buildInfo; };
  # qtwebview is dead weight that becomes a hard blocker under cross.
  #
  # It propagates qtwebengine -- a full Chromium -- and that does not
  # cross-evaluate to mingw: the failure surfaces as "Refusing to evaluate
  # package 'cups-2.4.19'", three levels away from the actual cause.
  #
  # Nothing uses it: no C++ include of QtWebView/QWebView, no `import QtWebView`
  # in any QML, and app/CMakeLists.txt's `find_package(Qt6 COMPONENTS ...)` list
  # omits WebView entirely. The only surviving mention is a stale line in
  # docs/project.md.
  #
  # Guarded rather than deleted so this stays a Windows-only change. Dropping it
  # on Unix as well would shrink every bundle and is worth doing separately.
  qtWebview = pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows) pkgs.qt6.qtwebview;
  qtWebviewQml = pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows)
    "${pkgs.qt6.qtwebview}/lib/qt-6/qml";

  # DLLs Windows itself provides. An import of one of these is satisfied out of
  # %SystemRoot%, never by us; everything NOT on this list has to ship with the
  # bundle. Compared with the extension stripped and case-folded, because import
  # tables mix spellings freely -- the real tables in this tree contain
  # "KERNEL32.dll", "IPHLPAPI.DLL" and "bcrypt.dll" side by side.
  #
  # Erring long here would hide a real gap, so entries are only added when a
  # measured import turns out to be an OS DLL. `api-ms-win-*` and `ext-ms-*`
  # (the API-set stubs) are matched by prefix in the shell.
  windowsSystemDlls = [
    "kernel32" "kernelbase" "ntdll" "user32" "gdi32" "gdiplus" "advapi32"
    "shell32" "shcore" "shlwapi" "ole32" "oleaut32" "oleacc" "comdlg32"
    "comctl32" "ws2_32" "wsock32" "mswsock" "crypt32" "bcrypt" "ncrypt"
    "secur32" "iphlpapi" "dbghelp" "version" "winmm" "imm32" "netapi32"
    "userenv" "dwmapi" "uxtheme" "d2d1" "d3d9" "d3d11" "d3d12" "dxgi"
    "dwrite" "opengl32" "glu32" "setupapi" "wtsapi32" "mpr" "rpcrt4"
    "msvcrt" "normaliz" "winspool" "odbc32" "authz" "dnsapi" "wldap32"
    "winhttp" "wininet" "psapi" "cfgmgr32" "uiautomationcore" "windowscodecs"
    "avicap32" "msimg32" "powrprof" "propsys" "usp10" "wintrust" "avrt"
  ];

  # Windows only. Every store path that may legitimately provide a DLL this
  # bundle needs, enumerated as a CLOSURE rather than as a hand-written list.
  #
  # A PE embeds no /nix/store strings, so Nix's reference scanner finds nothing
  # in a cross-built .exe or .dll and this derivation's own closure is useless
  # for the purpose: the providers have to be named from the build side. These
  # roots mirror `buildInputs` below plus the Logos components, and closureInfo
  # walks them transitively -- which matters, because the DLL that goes missing
  # is typically three or four levels down (libcurl -> libidn2 -> libiconv) and
  # no direct dependency of anything here.
  windowsDllClosure = pkgs.pkgsBuildBuild.closureInfo {
    rootPaths = common.buildInputs ++ [
      pkgs.qt6.qtdeclarative
      logosProtocolPkg
      logosQtHost
      logosLiblogos
      logosModule
      logosSdk
      logosDesignSystem
      logosViewModuleRuntime
    ] ++ installedModules;
  };
in
pkgs.stdenv.mkDerivation rec {
  pname = "logos-basecamp";
  version = common.version;

  inherit src;
  # Platform-specific build inputs for system webviews
  buildInputs = common.buildInputs ++ qtWebview ++ [
    pkgs.qt6.qtdeclarative
    # Qt host split: the app links logos-qt-host::logos_qt_host, which carries
    # the logos-protocol link interface (OpenSSL, Boost::system, nlohmann_json).
    logosProtocolPkg
    logosQtHost
    # app/CMakeLists.txt does find_package(LogosDesignSystem CONFIG REQUIRED).
    logosDesignSystem
  ] ++ (
    if pkgs.stdenv.isLinux then
      # Linux: WebKitGTK as backend + Wayland platform plugin
      [ webkitgtk pkgs.qt6.qtwayland ]
    else
      []
  );
  inherit (common) meta;

  # Add logosSdk to nativeBuildInputs for logos-cpp-generator
  nativeBuildInputs = common.nativeBuildInputs ++ [ logosSdkBuild pkgs.patchelf pkgs.removeReferencesTo ];

  # Provide Qt/GL runtime paths so the wrapper can inject them
  qtLibPath = pkgs.lib.makeLibraryPath (
    [
      pkgs.qt6.qtbase
      pkgs.qt6.qtremoteobjects
      pkgs.qt6.qtdeclarative
      pkgs.qt6.qtsvg
      pkgs.zstd
      pkgs.zlib
      pkgs.glib
      pkgs.stdenv.cc.cc
      pkgs.freetype
      pkgs.fontconfig
      # Qt host split: the app links logos-qt-host → logos-protocol, whose shared
      # runtime deps (Boost.System, OpenSSL) must be reachable now that the
      # binary's RPATH is stripped for bundling.
      pkgs.boost
      pkgs.openssl
    ]
    # See common.buildInputs: krb5 carries a host-platform bash and does not
    # cross-evaluate to mingw. makeLibraryPath is an ELF/Mach-O notion anyway.
    ++ pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows) pkgs.krb5
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
      pkgs.libglvnd
      pkgs.mesa.drivers
      pkgs.xorg.libX11
      pkgs.xorg.libXext
      pkgs.xorg.libXrender
      pkgs.xorg.libXrandr
      pkgs.xorg.libXcursor
      pkgs.xorg.libXi
      pkgs.xorg.libXfixes
      pkgs.xorg.libxcb
      pkgs.qt6.qtwayland
    ]
  );
  qtPluginPath = pkgs.lib.concatStringsSep ":" ([
    "${pkgs.qt6.qtbase}/lib/qt-6/plugins"
    "${pkgs.qt6.qtsvg}/lib/qt-6/plugins"
  ]
  ++ pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows)
    "${pkgs.qt6.qtwebview}/lib/qt-6/plugins"
  ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
    "${pkgs.qt6.qtwayland}/lib/qt-6/plugins"
  ]);
  qmlImportPath = pkgs.lib.concatStringsSep ":" ([
    "${placeholder "out"}/lib"
    "${pkgs.qt6.qtdeclarative}/lib/qt-6/qml"
  ] ++ qtWebviewQml ++ [
    "${pkgs.qt6.qtsvg}/lib/qt-6/qml"
  ]);

  preConfigure = ''
    runHook prePreConfigure

    # Set macOS deployment target to match Qt frameworks
    export MACOSX_DEPLOYMENT_TARGET=12.0

    # Copy logos-cpp-sdk headers to expected location
    echo "Copying logos-cpp-sdk headers for app..."
    mkdir -p ./logos-cpp-sdk/include/cpp
    cp -r ${logosSdk}/include/cpp/* ./logos-cpp-sdk/include/cpp/

    # core/interface.h ships with logos-qt-host (it moved to logos-qt-sdk in
    # the qt split, and on to the host runtime in the host split); the app
    # finds it via LOGOS_QT_HOST_ROOT, so nothing to stage here anymore.

    # Copy SDK library files to lib directory (no-op since the qt split — the
    # base SDK is header-only; kept for older layouts)
    echo "Copying SDK library files..."
    mkdir -p ./logos-cpp-sdk/lib
    if [ -f "${logosSdk}/lib/liblogos_sdk.dylib" ]; then
      cp "${logosSdk}/lib/liblogos_sdk.dylib" ./logos-cpp-sdk/lib/
    elif [ -f "${logosSdk}/lib/liblogos_sdk.so" ]; then
      cp "${logosSdk}/lib/liblogos_sdk.so" ./logos-cpp-sdk/lib/
    elif [ -f "${logosSdk}/lib/liblogos_sdk.a" ]; then
      cp "${logosSdk}/lib/liblogos_sdk.a" ./logos-cpp-sdk/lib/
    fi

    # ── app/generated: the ONE generated-header directory ──────────────────
    #
    # Keep it the only one: app/utils/BuildInfo.h resolves logos_build_info.h
    # with __has_include, so a second staged directory on the same include path
    # would leave -I ORDER deciding which header wins.
    mkdir -p ./app/generated

    # Auto-generated build info header (version + commit hashes): main.cpp logs
    # it at startup and MainUIBackend exposes it to the Dashboard.
    cp ${buildInfoHeader} ./app/generated/logos_build_info.h
    chmod +w ./app/generated/logos_build_info.h

    # Module-generated API headers. PackageCoordinator includes "logos_sdk.h",
    # whose umbrella pulls these in by bare name.
    echo "Copying include files from logos-package-manager-module..."
    if [ -d "${logosPackageManagerModule}/include" ]; then
      cp -r "${logosPackageManagerModule}/include"/* ./app/generated/
    else
      echo "Warning: No include directory found in logos-package-manager-module"
    fi

    echo "Copying include files from logos-package-downloader-module..."
    if [ -d "${logosPackageDownloaderModule}/include" ]; then
      cp -r "${logosPackageDownloaderModule}/include"/* ./app/generated/
    else
      echo "Warning: No include directory found in logos-package-downloader-module"
    fi

    # Shared semver headers (logos/semver.hpp + its <semver/semver.hpp>) so
    # AppsModel can use logos::semver::compare. Headers only — nothing here
    # links liblgx.
    cp -r "${logosPackageHeaders}/include/logos"  ./app/generated/
    cp -r "${logosPackageHeaders}/include/semver" ./app/generated/

    # logos-cpp-generator's general wrappers (logos_sdk.h / logos_sdk.cpp).
    # --general-only: the per-module wrappers come from the module outputs
    # copied above.
    echo "Running logos-cpp-generator (general-only)..."
    logos-cpp-generator --metadata ${src}/metadata.json --general-only --output-dir ./app/generated

    echo "Files in app/generated:"
    ls -la ./app/generated/

    runHook postPreConfigure
  '';

  # modules/ and plugins/ are carried into portable bundles by nix-bundle-dir.
  # extraClosurePaths lists Qt modules whose plugins/frameworks must be in
  # the bundle even though the app binary doesn't link against them directly
  # (they're used by portable-bundled plugins whose nix-store refs are stripped).
  passthru = {
    extraDirs = [ "modules" "plugins" ];
    extraClosurePaths = qtWebview ++ [ pkgs.qt6.qtsvg ]
      ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.qt6.qtwayland ]
      # Windows ONLY, and NOT because Windows needs extra Qt features -- it
      # needs the same ones, DECLARED differently.
      #
      # An ELF or Mach-O binary records where its dependencies live: the
      # qtdeclarative / libjpeg / sqlite store paths end up in rpaths and
      # install names, Nix scans them out as references, and closureInfo gets
      # all three without anyone naming them. A PE import table carries DLL
      # BASE NAMES only ("libjpeg-62.dll") and embeds no /nix/store string
      # anywhere, so Nix records NO reference and these paths never enter the
      # closure at all. The bundler cannot stage what is not in the closure.
      #
      # Measured -- each entry below is a build that FAILED without it, on the
      # real x86_64-windows Basecamp bundle, once flake.nix stopped bypassing
      # the bundler:
      #
      #   qtdeclarative  Phase 2b staged 17 Qt plugin files, then: "QtQuick/
      #                  QtQml DLLs are in bin/, so this bundle renders QML,
      #                  but no QML module directory for this target was found
      #                  in the closure." Adding it stages 1651 QML files and
      #                  lets the Phase 2e sweep pull in 40 more Qt DLLs.
      #   libjpeg.bin    "libjpeg-62.dll -- imported by
      #                  lib/qt-6/plugins/imageformats/qjpeg.dll".
      #   sqlite.bin     "libsqlite3-0.dll -- imported by
      #                  lib/qt-6/plugins/sqldrivers/qsqlite.dll".
      #
      # `.bin`, not the default output, is deliberate and was checked rather
      # than assumed: for both packages the mingw DLL is installed into the
      # `bin` output (`…-libjpeg-turbo-…-bin/bin/libjpeg-62.dll`,
      # `…-sqlite-…-bin/bin/libsqlite3-0.dll`), so rooting the closure at the
      # default output would add nothing and the build would fail unchanged.
      #
      # These last two are Qt's OWN plugin dependencies, not Basecamp's -- we
      # never call libjpeg or sqlite. They have to be declared here anyway,
      # because passthru.extraClosurePaths on the bundled derivation is the
      # only channel nix-bundle-dir reads.
      #
      # Windows-gated rather than unconditional so it provably cannot perturb
      # the Linux/macOS bundles, where the references already exist and adding
      # them would be a no-op that would still have to be re-proven.
      ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isWindows [
        pkgs.qt6.qtdeclarative
        pkgs.libjpeg.bin
        pkgs.sqlite.bin
      ];
  };

  # This is an aggregate runtime layout; avoid stripping to prevent hook errors
  dontStrip = true;

  # Skip wrapQtApps: we create our own wrapper for dev builds (hidden binary + shell launcher)
  # and portable builds don't need wrapping (nix-bundle-dir handles Qt paths)
  dontWrapQtApps = true;

  # Additional environment variables for Qt and RPATH cleanup
  preFixup = ''
    runHook prePreFixup

    # Set up Qt environment variables
    export QT_PLUGIN_PATH="${qtPluginPath}"
    export QML2_IMPORT_PATH="${pkgs.lib.concatStringsSep ":" ([
      "${pkgs.qt6.qtdeclarative}/lib/qt-6/qml"
    ] ++ qtWebviewQml ++ [
      "${pkgs.qt6.qtsvg}/lib/qt-6/qml"
    ])}"

    # Remove any remaining references to /build/ in binaries and set proper RPATH
    find $out -type f -executable -exec sh -c '
      if file "$1" | grep -q "ELF.*executable"; then
        # Use patchelf to clean up RPATH if it contains /build/
        if patchelf --print-rpath "$1" 2>/dev/null | grep -q "/build/"; then
          echo "Cleaning RPATH for $1"
          patchelf --remove-rpath "$1" 2>/dev/null || true
        fi
        # Set proper RPATH for the main binary
        if echo "$1" | grep -qE "/\.?LogosBasecamp$"; then
          echo "Setting RPATH for $1"
          patchelf --set-rpath "$out/lib" "$1" 2>/dev/null || true
        fi
      fi
    ' _ {} \;

    # Also clean up shared libraries
    find $out -name "*.so" -exec sh -c '
      if patchelf --print-rpath "$1" 2>/dev/null | grep -q "/build/"; then
        echo "Cleaning RPATH for $1"
        patchelf --remove-rpath "$1" 2>/dev/null || true
      fi
    ' _ {} \;

    runHook prePostFixup
  '';

  # Windows only: stage liblogos' OWN dependency DLLs, and then PROVE the closure.
  #
  # Copying ${logosLiblogos}/lib into bin/ (see installPhase) is necessary but not
  # sufficient. liblogos_core.dll and logos_host.exe each import libspdlog.dll and
  # libfmt.dll, and those two live in liblogos' BIN, not its lib/ -- nixpkgs'
  # win-dll-link.sh walked logos_host.exe's imports and staged the closure there.
  # spdlog is a buildInput of liblogos, not of basecamp, so basecamp's OWN
  # win-dll-link pass cannot find them either. Measured with a PE-capable objdump
  # over the built bundle: with lib/ copied and bin/ not, libspdlog.dll and
  # libfmt.dll were the only non-OS names left unresolved for LogosBasecamp.exe --
  # i.e. still 0xC0000135 before main(), the exact symptom copying lib/ cured for
  # liblogos_core itself. Mirrors what logos-package-manager/nix/lib.nix already
  # does with liblgx's closure.
  #
  # This is postFixup, NOT installPhase, and the ordering is load-bearing:
  # win-dll-link.sh registers _linkDLLs in fixupOutputHooks, which run BEFORE
  # postFixup. Doing it in installPhase instead makes the `already present` skip
  # below vacuous, so Qt6Core/Qt6Network/Qt6RemoteObjects get copied out of
  # liblogos as real files while Qt6Gui/Qt6Widgets stay symlinked into basecamp's
  # own qtbase -- a bin/ with a MIXED Qt in it the moment those two pins diverge.
  # Running after the hook lets the symlinks win and copies only what is genuinely
  # unprovided.
  postFixup = pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isWindows ''
    # Explicit `for` + `-f`, not a nullglob array: a fully interpolated literal
    # path contains no wildcard, so nullglob would leave it in the array and any
    # guard over it would pass vacuously.
    _dlldeps=0
    for dep in "${logosLiblogos}/bin/"*.dll; do
      [ -f "$dep" ] || continue
      # Anything win-dll-link.sh already resolved (symlink) or installPhase copied
      # stays; -e follows symlinks, which is what we want here.
      [ -e "$out/bin/$(basename "$dep")" ] && continue
      cp -L "$dep" "$out/bin/"
      _dlldeps=$((_dlldeps + 1))
    done
    echo "Staged $_dlldeps dependency DLL(s) from ${logosLiblogos}/bin"

    # Assert the invariant, not the mechanism: every DLL that liblogos_core.dll
    # imports AND that liblogos itself ships must now resolve next to the
    # executable. Names liblogos does not ship (kernel32, api-ms-*, ...) are OS
    # DLLs and are deliberately not checked, so this needs no hand-maintained
    # system-DLL list to stay correct.
    _objdump="''${OBJDUMP:-}"
    if [ -z "$_objdump" ] || ! command -v "$_objdump" >/dev/null 2>&1; then
      echo "ERROR: no \$OBJDUMP in this Windows-host stdenv; cannot verify the DLL closure" >&2
      exit 1
    fi
    _n_imports=$("$_objdump" -p "$out/bin/liblogos_core.dll" | grep -c 'DLL Name:')
    # A zero here means the objdump has no PE target, not that the DLL is
    # dependency-free. Treat it as a measurement failure, never as a pass.
    if [ "$_n_imports" -eq 0 ]; then
      echo "ERROR: $_objdump reported 0 imports for liblogos_core.dll (no PE target?)" >&2
      exit 1
    fi
    echo "liblogos_core.dll declares $_n_imports direct imports; checking the ones liblogos ships"
    _unmet=""
    for _imp in $("$_objdump" -p "$out/bin/liblogos_core.dll" | awk '/DLL Name:/{print $3}'); do
      if [ -e "${logosLiblogos}/bin/$_imp" ] || [ -e "${logosLiblogos}/lib/$_imp" ]; then
        [ -e "$out/bin/$_imp" ] || _unmet="$_unmet $_imp"
      fi
    done
    if [ -n "$_unmet" ]; then
      echo "ERROR: liblogos ships these DLLs but they did not reach $out/bin:$_unmet" >&2
      exit 1
    fi

    # ---------------------------------------------------------------------
    # Drive the WHOLE bundle's PE import closure to a fixpoint, then prove it.
    #
    # nixpkgs' win-dll-link.sh does walk imports to a fixpoint, but only over
    # `$prefix/bin` (its entry point is `_linkDLLs() { linkDLLsInfolder
    # "$prefix/bin"; }`), and it resolves each name against LINK_DLL_FOLDERS --
    # which is one level of buildInputs, contributed by an env hook. When a name
    # is not on that path it gives up in SILENCE:
    #
    #     readarray -d "" pathsFound < <(find "''${searchPaths[@]}" -name "$file" ...)
    #     if [ ''${#pathsFound[@]} -eq 0 ]; then continue; fi
    #
    # Two consequences, both of which shipped:
    #
    #  1. $out/modules/<m>/ and $out/plugins/<p>/ are populated in installPhase
    #     from .lgx payloads and UI-plugin outputs -- i.e. AFTER that hook has
    #     run, and in directories it never looks at. Nothing ever read their
    #     import tables. Measured before this block existed: main_ui.dll
    #     imported Qt6Qml.dll, Qt6QuickControls2.dll and Qt6QuickWidgets.dll and
    #     not one of them was in plugins/main_ui/ or in bin/.
    #
    #  2. A DLL that is itself absent cannot have ITS imports read, so one pass
    #     over a tree is never enough. package_downloader needs three rounds:
    #     package_downloader_plugin -> libpackage_downloader_lib -> libcurl-4 ->
    #     libidn2-0 -> libiconv-2.
    #
    # The failure mode is why this is a hard error and not a warning: a missing
    # DLL is ERROR_MOD_NOT_FOUND (126) / 0xC0000135 at LoadLibrary time, blamed
    # on the plugin rather than on the DLL that is absent, with no Qt message
    # and no stderr. It exits 0 at build time every single time.
    #
    # Windows searches the importing module's own directory first (logos-module
    # pre-loads with LOAD_WITH_ALTERED_SEARCH_PATH) and then the application
    # directory, so "resolved" here means: beside the importer, or in $out/bin.
    # Anything still unresolved is staged into $out/bin out of the closure.
    _sysdlls=${pkgs.lib.escapeShellArg (pkgs.lib.concatStringsSep " " windowsSystemDlls)}
    _is_system_dll() {
      local _b="''${1,,}" _s
      _b="''${_b%.dll}"; _b="''${_b%.drv}"; _b="''${_b%.exe}"
      case "$_b" in
        api-ms-win-*|ext-ms-*) return 0 ;;
      esac
      for _s in $_sysdlls; do [ "$_b" = "$_s" ] && return 0; done
      return 1
    }

    # Index the closure by lower-cased base name. bin/ wins over lib/ only by
    # first-writer; both are real providers under mingw and nothing in this tree
    # ships the same DLL twice with different contents.
    declare -A _dllsrc
    _indexed=0
    while IFS= read -r _sp; do
      [ -d "$_sp" ] || continue
      while IFS= read -r _f; do
        _b="$(basename "$_f")"; _k="''${_b,,}"
        if [ -z "''${_dllsrc[$_k]:-}" ]; then
          _dllsrc[$_k]="$_f"; _indexed=$((_indexed + 1))
        fi
      done < <(find "$_sp" -maxdepth 3 -type f -name '*.dll' 2>/dev/null)
    done < ${windowsDllClosure}/store-paths
    echo "Windows DLL index: $_indexed distinct name(s) across the build closure"
    # A zero index would make every "unresolved" below a measurement artefact
    # rather than a real gap, so it is fatal on its own.
    if [ "$_indexed" -eq 0 ]; then
      echo "ERROR: the build closure contains no .dll at all -- the closure is" >&2
      echo "wrong, or this is not a Windows build" >&2
      exit 1
    fi

    # `find -L`, and NO -maxdepth. Both were wrong before and both were silent:
    #
    #  * `-type f` does not match a SYMLINK, and 12 of the 36 PE entries in
    #    $out/bin are relative symlinks created by nixpkgs' win-dll-link.sh
    #    (Qt6Core, Qt6Gui, Qt6Widgets, Qt6Network, Qt6RemoteObjects,
    #    libcrypto/libssl, libzstd, libb2, libpng16, libpcre2,
    #    libdouble-conversion). Their import tables were never read, so four
    #    non-system names in this tree were reachable only through roots the
    #    gate could not see -- three of them entries on nix-bundle-lgx's
    #    windowsHostLibs, the list this gate is the only thing able to falsify.
    #    `-L` makes -type f test the TARGET, so a symlink to a real file matches.
    #  * `-maxdepth 1` skipped any PE below the first level of lib/, modules/<m>/
    #    or plugins/<p>/. Demonstrated: a DLL one directory deeper left the gate
    #    at rc=0 while a full-depth sweep found 8 unresolved imports.
    #
    # A dangling symlink is invisible to `find -L -type f`, so it is checked
    # separately below rather than being silently dropped from the root set.
    _pe_roots() {
      find -L "$out/bin" -type f \( -name '*.dll' -o -name '*.exe' \) 2>/dev/null || true
      [ -d "$out/lib" ] && { find -L "$out/lib" -type f -name '*.dll' 2>/dev/null || true; }
      for _d in "$out"/modules/* "$out"/plugins/*; do
        [ -d "$_d" ] || continue
        find -L "$_d" -type f \( -name '*.dll' -o -name '*.exe' \) 2>/dev/null || true
      done
    }

    _dangling=$(find "$out" -xtype l 2>/dev/null | wc -l)
    if [ "$_dangling" -ne 0 ]; then
      echo "ERROR: $_dangling dangling symlink(s) in the bundle:" >&2
      find "$out" -xtype l >&2
      echo "These are invisible to the import sweep below and unopenable at" >&2
      echo "runtime, so they would fail as 0xC0000135 with no output." >&2
      exit 1
    fi

    declare -A _unresolved
    _staged=0
    _imports_read=0
    _round=0
    while :; do
      _round=$((_round + 1))
      _added=0
      while IFS= read -r _root; do
        _rootdir="$(dirname "$_root")"
        while IFS= read -r _imp; do
          [ -n "$_imp" ] || continue
          _imports_read=$((_imports_read + 1))
          _is_system_dll "$_imp" && continue
          # -e, not -f: win-dll-link.sh's entries are relative symlinks.
          [ -e "$_rootdir/$_imp" ] && continue
          [ -e "$out/bin/$_imp" ] && continue
          _src="''${_dllsrc[''${_imp,,}]:-}"
          if [ -z "$_src" ]; then
            _unresolved["$_imp"]="''${_unresolved["$_imp"]:-}''${_root#$out/} "
            continue
          fi
          # cp -L, never cp -a: the provider is very often win-dll-link.sh's own
          # relative symlink into a third store path, and copying it as a link
          # stages something that dangles the moment the tree is moved.
          cp -L "$_src" "$out/bin/$_imp"
          chmod u+w "$out/bin/$_imp"
          echo "  round $_round  + $_imp  <- ''${_root#$out/}"
          _added=$((_added + 1)); _staged=$((_staged + 1))
        done < <("$_objdump" -p "$_root" 2>/dev/null | sed -n 's/.*DLL Name: *//p' | tr -d '\r')
      done < <(_pe_roots)
      [ "$_added" -eq 0 ] && break
      if [ "$_round" -ge 25 ]; then
        echo "ERROR: the DLL import closure did not reach a fixpoint in $_round rounds" >&2
        exit 1
      fi
    done
    echo "DLL import closure converged after $_round round(s); staged $_staged DLL(s) into bin/"

    # A zero here is the measurement bug this whole block exists to avoid: an
    # objdump without a PE target prints nothing and every import "resolves".
    if [ "$_imports_read" -eq 0 ]; then
      echo "ERROR: read 0 imports from the entire bundle -- $_objdump has no PE" >&2
      echo "target, or _pe_roots matched nothing. Not a pass." >&2
      exit 1
    fi

    # `+x`: under `set -u`, ''${#arr[@]} on an associative array that never
    # received an element is itself an error, which would fail the SUCCESS path.
    if [ -n "''${_unresolved[*]+x}" ]; then
      echo "" >&2
      echo "ERROR: ''${#_unresolved[@]} DLL import(s) cannot be resolved from this bundle:" >&2
      for _n in "''${!_unresolved[@]}"; do
        echo "  $_n" >&2
        printf '%s\n' ''${_unresolved["$_n"]} | sort -u | while IFS= read -r _i; do
          [ -n "$_i" ] && echo "      imported by: $_i" >&2
        done
      done
      echo "" >&2
      echo "A PE import table carries base names only, so this is not a degraded" >&2
      echo "feature: it is 0xC0000135 (STATUS_DLL_NOT_FOUND) before main() runs," >&2
      echo "or ERROR_MOD_NOT_FOUND blamed on the plugin, with no output at all." >&2
      echo "Two causes, in order of likelihood: (a) an .lgx payload dropped it," >&2
      echo "because nix-bundle-lgx's windowsHostLibs claims this bundle ships it" >&2
      echo "and it does not -- that list is an unverifiable promise about THIS" >&2
      echo "output, and this is the check that falsifies it; or (b) the providing" >&2
      echo "store path is absent from windowsDllClosure's rootPaths in app.nix." >&2
      exit 1
    fi
    # State the SIZE of what was checked, not just the verdict. The previous
    # message claimed full coverage of bin/, lib/, modules/ and plugins/ while
    # silently excluding 12 of 36 bin/ entries and everything below depth 1, and
    # because it was the only line on the pass path it was read as proof.
    echo "PE import closure verified: $(_pe_roots | wc -l) root(s), $_imports_read import(s) read, 0 unresolved."
  '';

  configurePhase = ''
    runHook preConfigure

    echo "Configuring logos-basecamp..."
    echo "liblogos: ${logosLiblogos}"
    echo "logos-module: ${logosModule}"
    echo "cpp-sdk: ${logosSdk}"
    echo "logos-design-system: ${logosDesignSystem}"

    # Verify that the built components exist
    test -d "${logosLiblogos}" || (echo "liblogos not found" && exit 1)
    test -d "${logosModule}" || (echo "logos-module not found" && exit 1)
    test -d "${logosSdk}" || (echo "cpp-sdk not found" && exit 1)
    test -d "${logosDesignSystem}" || (echo "logos-design-system not found" && exit 1)

    ${pkgs.lib.optionalString (enableInspector && logosQtMcp != null) ''
      echo "Copying logos-qt-mcp source for inspector..."
      mkdir -p ./logos-qt-mcp
      cp -r ${logosQtMcp}/* ./logos-qt-mcp/
    ''}

    # $cmakeFlags FIRST. This hand-rolled configurePhase bypasses the cmake
    # setup hook, so without it the cross-compilation flags nixpkgs computes are
    # silently dropped -- above all -DCMAKE_SYSTEM_NAME=Windows. The symptom is
    # nowhere near the cause: CMake's FindThreads then probes for pthreads
    # instead of Win32 threads, fails, and Qt6Config reports
    # "Qt6 could not be found because dependency Threads could not be found".
    # It also carries the Qt host-TOOL package paths (moc/rcc/qmltyperegistrar/
    # qsb), which -DQT_HOST_PATH cannot supply. Empty on native builds.
    cmake -S app -B build \
      $cmakeFlags \
      ${pkgs.lib.escapeShellArgs (pkgs.logosQtCrossCmakeFlags or [ ])} \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
      -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=FALSE \
      -DCMAKE_INSTALL_RPATH="" \
      -DCMAKE_SKIP_BUILD_RPATH=TRUE \
      -DLOGOS_MODULE_ROOT=${logosModule} \
      -DLOGOS_LIBLOGOS_ROOT=${logosLiblogos} \
      -DLOGOS_CPP_SDK_ROOT=$(pwd)/logos-cpp-sdk \
      -DLOGOS_QT_HOST_ROOT=${logosQtHost} \
      -DLOGOS_QT_SDK_ROOT=${logosQtSdk} \
      -DLOGOS_PROTOCOL_ROOT=${logosProtocolPkg} \
      -DLOGOS_VIEW_MODULE_RUNTIME_ROOT=${logosViewModuleRuntime} \
      -DLogosDesignSystem_DIR=${logosDesignSystem}/lib/cmake/LogosDesignSystem \
      -DLOGOS_DISTRIBUTED_BUILD=${if portable then "ON" else "OFF"} \
      -DLOGOS_PORTABLE_BUILD=${if portable then "ON" else "OFF"} \
      -DENABLE_QML_INSPECTOR=${if (enableInspector && logosQtMcp != null) then "ON" else "OFF"} \
      ${pkgs.lib.optionalString (enableInspector && logosQtMcp != null) "-DLOGOS_QT_MCP_ROOT=$(pwd)/logos-qt-mcp"}

    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild

    cmake --build build
    echo "logos-basecamp built successfully!"

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    # Create output directories
    mkdir -p $out/bin $out/lib $out/modules $out/plugins

    # Install app binary.
    #
    # Probe both names and FAIL if neither exists. The previous
    # `if [ -f build/LogosBasecamp ]` had no else-branch, so a mingw build --
    # which links build/LogosBasecamp.exe -- installed NOTHING, exited 0, and
    # produced an output whose bin/, lib/, modules/ and plugins/ were all empty.
    _bc=""
    for _cand in build/LogosBasecamp build/LogosBasecamp.exe; do
      if [ -f "$_cand" ]; then _bc="$_cand"; break; fi
    done
    if [ -z "$_bc" ]; then
      echo "Error: LogosBasecamp was not produced by the build" >&2
      ls -la build 2>&1 >&2 | head -40 || true
      exit 1
    fi
    if true; then
      ${if portable then ''
        # Portable: install binary directly (nix-bundle-dir handles Qt paths)
        cp "$_bc" "$out/bin/$(basename "$_bc")"
      '' else if pkgs.stdenv.hostPlatform.isWindows then ''
        # Windows: no shell wrapper -- a POSIX /bin/sh launcher cannot run
        # there, and Qt path setup belongs in a qt.conf beside the exe.
        cp "$_bc" "$out/bin/$(basename "$_bc")"
      '' else ''
        # Dev: hide real binary, create wrapper that sets Qt env vars
        cp "$_bc" "$out/bin/.LogosBasecamp"

        cat > $out/bin/LogosBasecamp << 'WRAPPER_EOF'
#!/bin/sh
BINDIR="$(cd "$(dirname "$0")" && pwd)"
APPDIR="$(cd "$BINDIR/.." && pwd)"
WRAPPER_EOF
        echo "export QT_PLUGIN_PATH=\"${qtPluginPath}\"" >> $out/bin/LogosBasecamp
        echo "export QML2_IMPORT_PATH=\"${qmlImportPath}\"" >> $out/bin/LogosBasecamp
        echo "export DYLD_LIBRARY_PATH=\"${qtLibPath}:\$DYLD_LIBRARY_PATH\"" >> $out/bin/LogosBasecamp
        echo "export LD_LIBRARY_PATH=\"${qtLibPath}:\$LD_LIBRARY_PATH\"" >> $out/bin/LogosBasecamp
        cat >> $out/bin/LogosBasecamp << 'WRAPPER_EOF'
if [ "$(uname)" = "Linux" ]; then
  export XDG_DATA_DIRS="$APPDIR/share''${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
  if [ -n "$WAYLAND_DISPLAY" ] && [ -z "$QT_QPA_PLATFORM" ]; then
    export QT_QPA_PLATFORM=wayland
  fi
fi
exec "$BINDIR/.LogosBasecamp" "$@"
WRAPPER_EOF
        chmod +x $out/bin/LogosBasecamp
      ''}
      echo "Installed LogosBasecamp"
    fi

    # Install ui-host binary from logos-view-module-runtime (process-isolated UI plugins)
    for _x in "" ".exe"; do
      if [ -f "${logosViewModuleRuntime}/bin/ui-host$_x" ]; then
        cp -L "${logosViewModuleRuntime}/bin/ui-host$_x" "$out/bin/ui-host$_x"
        echo "Installed ui-host$_x from logos-view-module-runtime"
        break
      fi
    done

    # Copy the core binaries from liblogos
    for _x in "" ".exe"; do
      if [ -f "${logosLiblogos}/bin/logoscore$_x" ]; then
        cp -L "${logosLiblogos}/bin/logoscore$_x" "$out/bin/"
        echo "Installed logoscore$_x"
        break
      fi
    done
    for _x in "" ".exe"; do
      if [ -f "${logosLiblogos}/bin/logos_host$_x" ]; then
        cp -L "${logosLiblogos}/bin/logos_host$_x" "$out/bin/"
        echo "Installed logos_host$_x"
        break
      fi
    done

    # Copy shared libraries from liblogos (includes logos_core and its dependency
    # package_manager_lib).
    #
    # The glob listed only *.dylib and *.so, so on Windows it matched NOTHING and
    # -- guarded by `[ -f ]` and `|| true` -- copied nothing while exiting 0. The
    # thirteen DLLs sitting in that same lib/ (liblogos_core, libpackage_manager_lib,
    # liblgx, icuuc76, icudt76, libsodium-26, ...) were silently dropped, and
    # LogosBasecamp.exe imports liblogos_core.dll DIRECTLY: the app died at
    # 0xC0000135 (STATUS_DLL_NOT_FOUND) before main(), which produces no Qt error,
    # no stderr, no output of any kind. It only ever ran because those DLLs were
    # hand-staged into the payload by the operator.
    #
    # DLLs go to bin/, NOT lib/. Windows searches the executable's own directory
    # first and has no rpath, and nixpkgs' win-dll-link.sh (which pulls in the rest
    # of the closure) only ever processes $out/bin.
    _libdest="$out/lib"
    ${pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isWindows ''_libdest="$out/bin"''}
    _copied=0
    for f in "${logosLiblogos}/lib/"*.dylib "${logosLiblogos}/lib/"*.so "${logosLiblogos}/lib/"*.dll; do
      # A non-matching glob stays literal, so test before copying.
      if [ -f "$f" ]; then
        cp -L "$f" "$_libdest/" || true
        _copied=$((_copied + 1))
      fi
    done
    echo "Installed $_copied shared librar(y|ies) from liblogos into $_libdest"
    # Assert rather than trust: this loop silently copying zero is exactly the
    # defect above, and it exits 0 either way.
    if [ "$_copied" -eq 0 ]; then
      echo "ERROR: copied no shared libraries from ${logosLiblogos}/lib" >&2
      ls -la "${logosLiblogos}/lib" >&2 || true
      exit 1
    fi

    # Copy SDK library if it exists
    # Test each candidate, do not `ls` the glob. With nullglob set (it is, in
    # this phase) an unmatched glob vanishes, so `ls` ran with NO arguments,
    # listed the working directory and succeeded -- after which `cp -L "$out/lib/"`
    # ran with a single argument. Measured in the mingw build log:
    #   cp: missing destination file operand after '.../lib/'
    # Guarded by `|| true`, so it exited 0 having copied nothing.
    for _sdklib in "${logosSdk}/lib/"liblogos_sdk.*; do
      [ -f "$_sdklib" ] || continue
      cp -L "$_sdklib" "$out/lib/"
    done

    # The UI shell. Staged from its own derivation into plugins/main_ui/, which
    # is where app/window.cpp resolves it and where the PE import sweep above
    # already walks ("$out"/plugins/*).
    ${pkgs.lib.optionalString (mainUIPlugin != null) ''
      if [ -d "${mainUIPlugin}/plugins" ]; then
        cp -r "${mainUIPlugin}/plugins/." "$out/plugins/"
        echo "Installed the main_ui shell plugin"
      else
        echo "error: mainUIPlugin produced no plugins/ directory"
        exit 1
      fi
    ''}

    # Copy pre-installed modules and plugins from bundled install outputs.
    # Each entry in installedModules has modules/ and/or plugins/ subdirectories.
    for installed in ${pkgs.lib.concatStringsSep " " (map toString installedModules)}; do
      if [ -d "$installed/modules" ]; then
        cp -rn "$installed/modules/." "$out/modules/"
      fi
      if [ -d "$installed/plugins" ]; then
        cp -rn "$installed/plugins/." "$out/plugins/"
      fi
    done
    echo "Pre-installed modules and plugins from install bundles"

    # Logos.Theme / .Icons / .Controls are STATIC-linked into the main_ui PLUGIN,
    # not into this binary, and register into the process-wide QML registry when
    # Window loads the plugin at startup — before any UI plugin can import them.
    # Exactly one image may link them: a STATIC qt_add_qml_module registers from
    # a static initializer and QML registration is process-global, so a second
    # image aborts startup with "Cannot add multiple registrations for
    # Logos.Icons". Nothing to copy to $out/lib/Logos.

    # Install desktop file and icon for FreeDesktop / Wayland icon lookup (Linux only)
    if [ "$(uname)" = "Linux" ]; then
      mkdir -p $out/share/applications $out/share/icons/hicolor/256x256/apps
      cp ${src}/assets/logos-basecamp.desktop $out/share/applications/
      cp ${src}/app/icons/logos.png $out/share/icons/hicolor/256x256/apps/logos-basecamp.png
    fi

    # Create a README for reference
    cat > $out/README.txt <<EOF
Logos Basecamp - Build Information
==================================
liblogos: ${logosLiblogos}
cpp-sdk: ${logosSdk}
logos-design-system: ${logosDesignSystem}

Runtime Layout:
- Entry point: $out/bin/LogosBasecamp
- Libraries: $out/lib
- Embedded modules: $out/modules (pre-installed at build time)
- Embedded plugins: $out/plugins (pre-installed at build time)

Usage:
  $out/bin/LogosBasecamp
EOF

    runHook postInstall
  '';

}
