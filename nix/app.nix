# Builds the logos-basecamp standalone application
{ pkgs, common, src, logosModule, logosLiblogos, logosSdk, logosSdkBuild ? logosSdk, logosProtocolPkg, logosQtSdk, logosDesignSystem, logosViewModuleRuntime, buildInfo, logosQtMcp ? null, installedModules ? [], portable ? false, enableInspector ? true }:

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
in
pkgs.stdenv.mkDerivation rec {
  pname = "logos-basecamp";
  version = common.version;

  inherit src;
  # Platform-specific build inputs for system webviews
  buildInputs = common.buildInputs ++ qtWebview ++ [
    pkgs.qt6.qtdeclarative
    # Qt split: the app links logos-qt-sdk::logos_qt_sdk, which carries the
    # logos-protocol link interface (OpenSSL, Boost::system, nlohmann_json).
    logosProtocolPkg
    logosQtSdk
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
      # Qt split: the app links logos-qt-sdk → logos-protocol, whose shared
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

    # core/interface.h moved to logos-qt-sdk in the qt split; the app finds it
    # via LOGOS_QT_SDK_ROOT, so nothing to stage here anymore.

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

    # Drop the auto-generated build info header (version + commit hashes) so
    # main.cpp can log it at startup.
    mkdir -p ./app/generated
    cp ${buildInfoHeader} ./app/generated/logos_build_info.h
    chmod +w ./app/generated/logos_build_info.h

    runHook postPreConfigure
  '';

  # modules/ and plugins/ are carried into portable bundles by nix-bundle-dir.
  # extraClosurePaths lists Qt modules whose plugins/frameworks must be in
  # the bundle even though the app binary doesn't link against them directly
  # (they're used by portable-bundled plugins whose nix-store refs are stripped).
  passthru = {
    extraDirs = [ "modules" "plugins" ];
    extraClosurePaths = qtWebview ++ [ pkgs.qt6.qtsvg ]
      ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.qt6.qtwayland ];
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
      -DLOGOS_QT_SDK_ROOT=${logosQtSdk} \
      -DLOGOS_PROTOCOL_ROOT=${logosProtocolPkg} \
      -DLOGOS_VIEW_MODULE_RUNTIME_ROOT=${logosViewModuleRuntime} \
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
    if ls "${logosSdk}/lib/"liblogos_sdk.* >/dev/null 2>&1; then
      cp -L "${logosSdk}/lib/"liblogos_sdk.* "$out/lib/" || true
    fi

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

    # Logos.Theme / .Icons / .Controls are STATIC-linked into main_ui.dylib
    # via find_package(LogosDesignSystem CONFIG) — the modules register into
    # the process qrc at load time. Nothing to copy to $out/lib/Logos.

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
