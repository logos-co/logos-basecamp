# Builds the logos-basecamp standalone application
{ pkgs, common, src, logosModule, logosLiblogos, logosSdk, logosDesignSystem, logosViewModuleRuntime, buildInfo, logosQtMcp ? null, installedModules ? [], portable ? false, enableInspector ? true }:

let
  # webkitgtk became ABI-versioned; pick the newest available while staying
  # compatible with older nixpkgs where the unversioned attribute still exists.
  webkitgtk = pkgs.webkitgtk_4_1 or pkgs.webkitgtk_4_0 or pkgs.webkitgtk;

  buildInfoHeader = import ./build-info.nix { inherit pkgs buildInfo; };
in
pkgs.stdenv.mkDerivation rec {
  pname = "logos-basecamp";
  version = common.version;

  inherit src;
  # Platform-specific build inputs for system webviews
  buildInputs = common.buildInputs ++ [
    pkgs.qt6.qtwebview
    pkgs.qt6.qtdeclarative
  ] ++ (
    if pkgs.stdenv.isLinux then
      # Linux: WebKitGTK as backend + Wayland platform plugin
      [ webkitgtk pkgs.qt6.qtwayland ]
    else
      []
  );
  inherit (common) meta;

  # Add logosSdk to nativeBuildInputs for logos-cpp-generator
  nativeBuildInputs = common.nativeBuildInputs ++ [ logosSdk pkgs.patchelf pkgs.removeReferencesTo ];

  # Provide Qt/GL runtime paths so the wrapper can inject them
  qtLibPath = pkgs.lib.makeLibraryPath (
    [
      pkgs.qt6.qtbase
      pkgs.qt6.qtremoteobjects
      pkgs.qt6.qtwebview
      pkgs.qt6.qtdeclarative
      pkgs.qt6.qtsvg
      pkgs.zstd
      pkgs.krb5
      pkgs.zlib
      pkgs.glib
      pkgs.stdenv.cc.cc
      pkgs.freetype
      pkgs.fontconfig
    ]
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
    "${pkgs.qt6.qtwebview}/lib/qt-6/plugins"
    "${pkgs.qt6.qtsvg}/lib/qt-6/plugins"
  ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
    "${pkgs.qt6.qtwayland}/lib/qt-6/plugins"
  ]);
  qmlImportPath = "${placeholder "out"}/lib:${pkgs.qt6.qtdeclarative}/lib/qt-6/qml:${pkgs.qt6.qtwebview}/lib/qt-6/qml:${pkgs.qt6.qtsvg}/lib/qt-6/qml";

  preConfigure = ''
    runHook prePreConfigure

    # Set macOS deployment target to match Qt frameworks
    export MACOSX_DEPLOYMENT_TARGET=12.0

    # Copy logos-cpp-sdk headers to expected location
    echo "Copying logos-cpp-sdk headers for app..."
    mkdir -p ./logos-cpp-sdk/include/cpp
    cp -r ${logosSdk}/include/cpp/* ./logos-cpp-sdk/include/cpp/

    # Also copy core headers
    echo "Copying core headers..."
    mkdir -p ./logos-cpp-sdk/include/core
    cp -r ${logosSdk}/include/core/* ./logos-cpp-sdk/include/core/

    # Copy SDK library files to lib directory
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
    extraClosurePaths = [ pkgs.qt6.qtwebview pkgs.qt6.qtsvg ]
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
    export QML2_IMPORT_PATH="${pkgs.qt6.qtdeclarative}/lib/qt-6/qml:${pkgs.qt6.qtwebview}/lib/qt-6/qml:${pkgs.qt6.qtsvg}/lib/qt-6/qml"

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

    cmake -S app -B build \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
      -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=FALSE \
      -DCMAKE_INSTALL_RPATH="" \
      -DCMAKE_SKIP_BUILD_RPATH=TRUE \
      -DLOGOS_MODULE_ROOT=${logosModule} \
      -DLOGOS_LIBLOGOS_ROOT=${logosLiblogos} \
      -DLOGOS_CPP_SDK_ROOT=$(pwd)/logos-cpp-sdk \
      -DLOGOS_VIEW_MODULE_RUNTIME_ROOT=${logosViewModuleRuntime} \
      -DLOGOS_PORTABLE_BUILD=${if portable then "ON" else "OFF"} \
      -DENABLE_QML_INSPECTOR=${if enableInspector then "ON" else "OFF"} \
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

    # Install app binary
    if [ -f "build/LogosBasecamp" ]; then
      ${if portable then ''
        # Portable: install binary directly (nix-bundle-dir handles Qt paths)
        cp build/LogosBasecamp "$out/bin/LogosBasecamp"
      '' else ''
        # Dev: hide real binary, create wrapper that sets Qt env vars
        cp build/LogosBasecamp "$out/bin/.LogosBasecamp"

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
    if [ -f "${logosViewModuleRuntime}/bin/ui-host" ]; then
      cp "${logosViewModuleRuntime}/bin/ui-host" "$out/bin/ui-host"
      echo "Installed ui-host binary from logos-view-module-runtime"
    fi

    # Copy the core binaries from liblogos
    if [ -f "${logosLiblogos}/bin/logoscore" ]; then
      cp -L "${logosLiblogos}/bin/logoscore" "$out/bin/"
      echo "Installed logoscore binary"
    fi
    if [ -f "${logosLiblogos}/bin/logos_host" ]; then
      cp -L "${logosLiblogos}/bin/logos_host" "$out/bin/"
      echo "Installed logos_host binary"
    fi

    # Copy shared libraries from liblogos (includes logos_core and its dependency package_manager_lib)
    for f in "${logosLiblogos}/lib/"*.dylib "${logosLiblogos}/lib/"*.so; do
      if [ -f "$f" ]; then
        cp -L "$f" "$out/lib/" || true
      fi
    done

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

    # Copy design system QML modules (Logos.Theme, Logos.Controls, Logos.Icons) for runtime
    if [ -d "${logosDesignSystem}/lib/Logos/Theme" ]; then
      mkdir -p "$out/lib/Logos"
      cp -R "${logosDesignSystem}/lib/Logos/Theme" "$out/lib/Logos/"
      echo "Copied Logos.Theme to lib/Logos/Theme/"
    fi
    if [ -d "${logosDesignSystem}/lib/Logos/Controls" ]; then
      mkdir -p "$out/lib/Logos"
      cp -R "${logosDesignSystem}/lib/Logos/Controls" "$out/lib/Logos/"
      echo "Copied Logos.Controls to lib/Logos/Controls/"
    fi
    if [ -d "${logosDesignSystem}/lib/Logos/Icons" ]; then
      mkdir -p "$out/lib/Logos"
      cp -R "${logosDesignSystem}/lib/Logos/Icons" "$out/lib/Logos/"
      echo "Copied Logos.Icons to lib/Logos/Icons/"
    fi

    # Install desktop file and icon for FreeDesktop / Wayland icon lookup (Linux only)
    if [ "$(uname)" = "Linux" ]; then
      mkdir -p $out/share/applications $out/share/icons/hicolor/256x256/apps
      cp ${src}/assets/logos-basecamp.desktop $out/share/applications/
      cp ${src}/app/icons/logos.png $out/share/icons/hicolor/256x256/apps/logos-basecamp.png
    fi

    # Create a README for reference
    cat > $out/README.txt <<EOF
Logos App - Build Information
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
