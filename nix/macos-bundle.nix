# Creates a macOS .app bundle for LogosApp
{ pkgs, app, src }:

pkgs.stdenv.mkDerivation rec {
  pname = "LogosApp";
  version = "1.0.0";
  
  dontUnpack = true;
  dontWrapQtApps = true;
  
  nativeBuildInputs = [ pkgs.makeWrapper ];
  buildInputs = [ pkgs.qt6.qtbase pkgs.qt6.qtdeclarative ];
  
  appSrc = src;
  
  installPhase = ''
    runHook preInstall
    
    mkdir -p "$out/LogosApp.app/Contents/MacOS"
    mkdir -p "$out/LogosApp.app/Contents/Frameworks"
    mkdir -p "$out/LogosApp.app/Contents/Resources"
    mkdir -p "$out/LogosApp.app/Contents/PlugIns"
    mkdir -p "$out/LogosApp.app/Contents/preinstall"
    
    if [ -f "${app}/bin/.LogosApp-wrapped" ]; then
      cp -L "${app}/bin/.LogosApp-wrapped" "$out/LogosApp.app/Contents/MacOS/LogosApp"
    else
      cp -L "${app}/bin/LogosApp" "$out/LogosApp.app/Contents/MacOS/"
    fi
    chmod +x "$out/LogosApp.app/Contents/MacOS/LogosApp"
    
    if [ -f "${app}/bin/logos_host" ]; then
      cp -L "${app}/bin/logos_host" "$out/LogosApp.app/Contents/MacOS/"
      chmod +x "$out/LogosApp.app/Contents/MacOS/logos_host"
    fi
    if [ -f "${app}/bin/logoscore" ]; then
      cp -L "${app}/bin/logoscore" "$out/LogosApp.app/Contents/MacOS/"
      chmod +x "$out/LogosApp.app/Contents/MacOS/logoscore"
    fi
    
    if [ -d "${app}/lib" ]; then
      for lib in "${app}/lib"/*.dylib; do
        if [ -f "$lib" ]; then
          cp -L "$lib" "$out/LogosApp.app/Contents/Frameworks/"
        fi
      done
    fi
    
    qtbase="${pkgs.qt6.qtbase}"
    for framework in QtCore QtGui QtWidgets QtDBus; do
      if [ -d "$qtbase/lib/$framework.framework" ]; then
        cp -RL "$qtbase/lib/$framework.framework" "$out/LogosApp.app/Contents/Frameworks/"
      fi
    done
    
    qtdeclarative="${pkgs.qt6.qtdeclarative}"
    for framework in QtQml QtQuick QtQmlModels; do
      if [ -d "$qtdeclarative/lib/$framework.framework" ]; then
        cp -RL "$qtdeclarative/lib/$framework.framework" "$out/LogosApp.app/Contents/Frameworks/"
      fi
    done
    
    if [ -d "$qtbase/lib/qt-6/plugins" ]; then
      cp -RL "$qtbase/lib/qt-6/plugins/platforms" "$out/LogosApp.app/Contents/PlugIns/" || true
      cp -RL "$qtbase/lib/qt-6/plugins/styles" "$out/LogosApp.app/Contents/PlugIns/" || true
      cp -RL "$qtbase/lib/qt-6/plugins/imageformats" "$out/LogosApp.app/Contents/PlugIns/" || true
    fi
    
    mkdir -p "$out/LogosApp.app/Contents/Resources/qml"
    if [ -d "$qtdeclarative/lib/qt-6/qml" ]; then
      cp -RL "$qtdeclarative/lib/qt-6/qml"/* "$out/LogosApp.app/Contents/Resources/qml/" || true
    fi
    
    # Copy Logos design system QML modules (Theme, Controls)
    if [ -d "${app}/lib/Logos/Theme" ]; then
      mkdir -p "$out/LogosApp.app/Contents/Resources/qml/Logos"
      cp -R "${app}/lib/Logos/Theme" "$out/LogosApp.app/Contents/Resources/qml/Logos/"
      echo "Copied Logos.Theme to Resources/qml/Logos/Theme/"
    fi
    if [ -d "${app}/lib/Logos/Controls" ]; then
      mkdir -p "$out/LogosApp.app/Contents/Resources/qml/Logos"
      cp -R "${app}/lib/Logos/Controls" "$out/LogosApp.app/Contents/Resources/qml/Logos/"
      echo "Copied Logos.Controls to Resources/qml/Logos/Controls/"
    fi
    
    if [ -d "$qtdeclarative/lib/qt-6/plugins" ]; then
      cp -RL "$qtdeclarative/lib/qt-6/plugins"/* "$out/LogosApp.app/Contents/PlugIns/" || true
    fi
    
    # Copy preinstall lgx packages (installed into user data dir on first launch)
    if [ -d "${app}/preinstall" ]; then
      cp "${app}/preinstall"/*.lgx "$out/LogosApp.app/Contents/preinstall/"
    fi
    
    cp "${appSrc}/app/macos/logos.icns" "$out/LogosApp.app/Contents/Resources/"
    
    cat > "$out/LogosApp.app/Contents/Resources/qt.conf" <<EOF
[Paths]
Plugins = PlugIns
Qml2Imports = Resources/qml
EOF
    
    sed -e "s/@VERSION@/${version}/g" \
        -e "s/@BUILD_NUMBER@/1/g" \
        "${appSrc}/app/macos/Info.plist.in" > "$out/LogosApp.app/Contents/Info.plist"
    
    echo -n "APPL????" > "$out/LogosApp.app/Contents/PkgInfo"
    
    for lib in "$out/LogosApp.app/Contents/Frameworks"/*.dylib; do
      if [ -f "$lib" ]; then
        libname=$(basename "$lib")
        install_name_tool -id "@executable_path/../Frameworks/$libname" "$lib" 2>/dev/null || true
        install_name_tool -change "${app}/lib/$libname" "@executable_path/../Frameworks/$libname" "$out/LogosApp.app/Contents/MacOS/LogosApp" 2>/dev/null || true
      fi
    done
    
    for framework in QtCore QtGui QtWidgets QtDBus; do
      install_name_tool -change "$qtbase/lib/$framework.framework/Versions/A/$framework" \
        "@executable_path/../Frameworks/$framework.framework/Versions/A/$framework" \
        "$out/LogosApp.app/Contents/MacOS/LogosApp" 2>/dev/null || true
    done
    
    for framework in QtQml QtQuick QtQmlModels; do
      install_name_tool -change "$qtdeclarative/lib/$framework.framework/Versions/A/$framework" \
        "@executable_path/../Frameworks/$framework.framework/Versions/A/$framework" \
        "$out/LogosApp.app/Contents/MacOS/LogosApp" 2>/dev/null || true
    done
    
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$out/LogosApp.app/Contents/MacOS/LogosApp" 2>/dev/null || true
    
    /usr/bin/codesign --force --deep --sign - "$out/LogosApp.app" 2>/dev/null || echo "Codesigning skipped (requires macOS)"
    
    ln -s "LogosApp.app/Contents/MacOS/LogosApp" "$out/LogosApp"
    
    runHook postInstall
  '';
  
  meta = with pkgs.lib; {
    description = "LogosApp macOS Application Bundle";
    platforms = platforms.darwin;
  };
}
