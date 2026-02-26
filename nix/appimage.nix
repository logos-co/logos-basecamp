# Builds a Linux AppImage for LogosApp
{ pkgs, app, version, src }:

assert pkgs.stdenv.isLinux;

let
  # Fetch the AppImage runtime from the official source
  appimageRuntime = pkgs.fetchurl {
    url = "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64";
    sha256 = "sha256-J93T945IP8X3hW5BPXwXCSkX+MNb/jMYoNN4qpQ1rRc=";
  };

  runtimeLibs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    pkgs.qt6.qtwebview
    pkgs.qt6.qtdeclarative
    pkgs.zstd
    pkgs.krb5
    pkgs.zlib
    pkgs.glib
    pkgs.freetype
    pkgs.fontconfig
    pkgs.libglvnd
    pkgs.mesa
    pkgs.xorg.libX11
    pkgs.xorg.libXext
    pkgs.xorg.libXrender
    pkgs.xorg.libXrandr
    pkgs.xorg.libXcursor
    pkgs.xorg.libXi
    pkgs.xorg.libXfixes
    pkgs.xorg.libxcb
  ];
  runtimeLibsStr = pkgs.lib.concatStringsSep " " (map toString runtimeLibs);
in
pkgs.stdenv.mkDerivation rec {
  pname = "logos-app-appimage";
  inherit version;

  dontUnpack = true;
  dontWrapQtApps = true;  # We handle Qt paths manually in AppRun
  nativeBuildInputs = [ pkgs.squashfsTools ];
  buildInputs = runtimeLibs;

  installPhase = ''
    set -euo pipefail
    appDir=$out/LogosApp.AppDir
    mkdir -p "$appDir/usr"

    # Application payload (use -rL to dereference symlinks and not preserve read-only perms)
    cp -rL --no-preserve=mode ${app}/bin "$appDir/usr/"
    chmod +x "$appDir/usr/bin/"*
    if [ -d ${app}/lib ]; then cp -rL --no-preserve=mode ${app}/lib "$appDir/usr/"; fi
    if [ -d ${app}/preinstall ]; then cp -rL --no-preserve=mode ${app}/preinstall "$appDir/usr/"; fi

    mkdir -p "$appDir/usr/lib"
    for dep in ${runtimeLibsStr}; do
      if [ -d "$dep/lib" ]; then
        cp -L --no-preserve=mode "$dep"/lib/*.so* "$appDir/usr/lib/" 2>/dev/null || true
        if [ -d "$dep/lib/qt-6" ]; then
          cp -rL --no-preserve=mode "$dep/lib/qt-6" "$appDir/usr/lib/" 2>/dev/null || true
        fi
      fi
    done

    # Qt plugins and QML modules
    mkdir -p "$appDir/usr/lib/qt-6/plugins" "$appDir/usr/lib/qt-6/qml"
    cp -rL --no-preserve=mode ${pkgs.qt6.qtbase}/lib/qt-6/plugins/* "$appDir/usr/lib/qt-6/plugins/" 2>/dev/null || true
    cp -rL --no-preserve=mode ${pkgs.qt6.qtwebview}/lib/qt-6/plugins/* "$appDir/usr/lib/qt-6/plugins/" 2>/dev/null || true
    cp -rL --no-preserve=mode ${pkgs.qt6.qtdeclarative}/lib/qt-6/qml/* "$appDir/usr/lib/qt-6/qml/" 2>/dev/null || true
    cp -rL --no-preserve=mode ${pkgs.qt6.qtwebview}/lib/qt-6/qml/* "$appDir/usr/lib/qt-6/qml/" 2>/dev/null || true

    # Desktop entry and icon
    mkdir -p "$appDir/usr/share/applications" "$appDir/usr/share/icons/hicolor/256x256/apps"
    cat > "$appDir/usr/share/applications/logos-app.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Logos App
Exec=LogosApp
Icon=logos-app
Categories=Utility;
EOF
    cp ${src}/app/icons/logos.png "$appDir/usr/share/icons/hicolor/256x256/apps/logos-app.png"
    ln -sf usr/share/icons/hicolor/256x256/apps/logos-app.png "$appDir/.DirIcon"

    # AppRun launcher to wire up runtime paths
    cat > "$appDir/AppRun" <<'EOF'
#!/bin/sh
APPDIR="$(dirname "$(readlink -f "$0")")"
export PATH="$APPDIR/usr/bin:$PATH"
export LD_LIBRARY_PATH="$APPDIR/usr/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$APPDIR/usr/lib/qt-6/plugins''${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export QML2_IMPORT_PATH="$APPDIR/usr/lib/qt-6/qml''${QML2_IMPORT_PATH:+:$QML2_IMPORT_PATH}"
export NIXPKGS_QT6_QML_IMPORT_PATH="$QML2_IMPORT_PATH"
exec "$APPDIR/usr/bin/LogosApp" "$@"
EOF
    chmod +x "$appDir/AppRun"

    # Create desktop file symlink at root (required by AppImage spec)
    ln -sf usr/share/applications/logos-app.desktop "$appDir/logos-app.desktop"

    # Build the AppImage using squashfs and the runtime
    mksquashfs "$appDir" "$out/squashfs.img" -root-owned -noappend -comp zstd -Xcompression-level 22
    
    # Combine runtime + squashfs into final AppImage
    mkdir -p "$out"
    cat ${appimageRuntime} "$out/squashfs.img" > "$out/LogosApp-${version}.AppImage"
    chmod +x "$out/LogosApp-${version}.AppImage"
    
    # Clean up intermediate file
    rm "$out/squashfs.img"
  '';

  meta = with pkgs.lib; {
    description = "Logos App AppImage";
    platforms = platforms.linux;
    mainProgram = "LogosApp";
  };
}
