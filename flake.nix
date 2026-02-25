{
  description = "Logos App - Qt application with UI plugins";

  inputs = {
    # Follow the same nixpkgs as logos-liblogos to ensure compatibility
    nixpkgs.follows = "logos-liblogos/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-package-manager.url = "github:logos-co/logos-package-manager-module";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    logos-package.url = "github:logos-co/logos-package/add-manifest-json-getter";
    logos-package-manager-ui.url = "github:logos-co/logos-package-manager-ui";
    logos-webview-app.url = "github:logos-co/logos-webview-app";
    logos-design-system.url = "github:logos-co/logos-design-system";
    logos-counter-qml.url = "github:logos-co/counter_qml";
    logos-counter.url = "github:logos-co/counter";
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
  };

  outputs = { self, nixpkgs, logos-cpp-sdk, logos-liblogos, logos-package-manager, logos-capability-module, logos-package, logos-package-manager-ui, logos-webview-app, logos-design-system, logos-counter-qml, logos-counter, nix-bundle-lgx }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosLiblogos = logos-liblogos.packages.${system}.default;
        logosPackageManager = logos-package-manager.packages.${system}.default;
        logosPackageManagerLib = logos-package-manager.packages.${system}.lib;
        logosCapabilityModule = logos-capability-module.packages.${system}.default;
        logosPackageLib = logos-package.packages.${system}.lib;
        logosPackageManagerUI = logos-package-manager-ui.packages.${system}.default;
        logosPackageManagerUIDistributed = logos-package-manager-ui.packages.${system}.distributed;
        logosWebviewApp = logos-webview-app.packages.${system}.default;
        logosDesignSystem = logos-design-system.packages.${system}.default;
        logosCounterQml = logos-counter-qml.packages.${system}.default;
        logosCounter = logos-counter.packages.${system}.default;
        logosCppSdkSrc = logos-cpp-sdk.outPath;
        logosLiblogosSrc = logos-liblogos.outPath;
        logosPackageManagerSrc = logos-package-manager.outPath;
        logosCapabilityModuleSrc = logos-capability-module.outPath;
        bundleLgx = nix-bundle-lgx.bundlers.${system}.default;
        bundleLgxPortable = nix-bundle-lgx.bundlers.${system}.portable;
      });
    in
    {
      packages = forAllSystems ({ pkgs, logosSdk, logosLiblogos, logosPackageManager, logosPackageManagerLib, logosCapabilityModule, logosPackageLib, logosPackageManagerUI, logosPackageManagerUIDistributed, logosWebviewApp, logosDesignSystem, logosCounterQml, logosCounter, bundleLgx, bundleLgxPortable, ... }:
        let
          # Common configuration
          common = import ./nix/default.nix {
            inherit pkgs logosSdk logosLiblogos;
          };
          src = ./.;

          # Plugin packages (development builds)
          counterPlugin = logosCounter;
          counterQmlPlugin = logosCounterQml;
          mainUIPlugin = import ./nix/main-ui.nix {
            inherit pkgs common src logosSdk logosPackageManager logosLiblogos;
          };
          packageManagerUIPlugin = logosPackageManagerUI;
          webviewAppPlugin = logosWebviewApp;

          # Plugin packages (distributed builds for DMG/AppImage)
          mainUIPluginDistributed = import ./nix/main-ui.nix {
            inherit pkgs common src logosSdk logosPackageManager logosLiblogos;
            distributed = true;
          };
          packageManagerUIPluginDistributed = logosPackageManagerUIDistributed;

          # LGX preinstall packages — installed on first app launch via the package manager.
          # Dev build: raw derivation (depends on /nix/store at runtime).
          # Distributed build: portable self-contained bundle (nix-bundle-dir pre-applied).
          preinstallPkgsDev = map bundleLgx [
            logosPackageManagerLib
            logosCapabilityModule
            counterPlugin
            counterQmlPlugin
            mainUIPlugin
            packageManagerUIPlugin
            webviewAppPlugin
          ];
          preinstallPkgsDistributed = map bundleLgxPortable [
            logosPackageManagerLib
            logosCapabilityModule
            counterPlugin
            counterQmlPlugin
            mainUIPluginDistributed
            packageManagerUIPluginDistributed
            webviewAppPlugin
          ];

          # App package (development build)
          app = import ./nix/app.nix {
            inherit pkgs common src logosLiblogos logosSdk logosDesignSystem logosPackageManager;
            preinstallPkgs = preinstallPkgsDev;
          };

          # App package (distributed build for DMG/AppImage)
          appDistributed = import ./nix/app.nix {
            inherit pkgs common src logosLiblogos logosSdk logosDesignSystem logosPackageManager;
            preinstallPkgs = preinstallPkgsDistributed;
            portable = true;
          };
          
          # macOS distribution packages (only for Darwin)
          appBundle = if pkgs.stdenv.isDarwin then
            import ./nix/macos-bundle.nix {
              inherit pkgs src;
              app = appDistributed;
            }
          else null;
          
          dmg = if pkgs.stdenv.isDarwin then
            import ./nix/macos-dmg.nix {
              inherit pkgs;
              appBundle = appBundle;
            }
          else null;

          # Linux AppImage (only for Linux)
          appImage = if pkgs.stdenv.isLinux then
            import ./nix/appimage.nix {
              inherit pkgs src;
              app = appDistributed;
              version = common.version;
            }
          else null;
        in
        {
          # Individual outputs
          counter-plugin = counterPlugin;
          counter-qml-plugin = counterQmlPlugin;
          main-ui-plugin = mainUIPlugin;
          package-manager-ui-plugin = packageManagerUIPlugin;
          webview-app-plugin = webviewAppPlugin;
          app = app;
          portable = appDistributed;
          
          # Default package
          default = app;
        } // (if pkgs.stdenv.isDarwin then {
          # macOS distribution outputs
          app-bundle = appBundle;
          inherit dmg;
        } else {}) // (if pkgs.stdenv.isLinux then {
          # Linux distribution output
          appimage = appImage;
        } else {})
      );

      devShells = forAllSystems ({ pkgs, logosSdk, logosLiblogos, logosPackageManager, logosCapabilityModule, logosPackageLib, logosDesignSystem, logosCppSdkSrc, logosLiblogosSrc, logosPackageManagerSrc, logosCapabilityModuleSrc }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.zstd
            pkgs.krb5
            pkgs.abseil-cpp
          ];
          
          shellHook = ''
            # Nix package paths (pre-built for host system)
            export LOGOS_CPP_SDK_ROOT="${logosSdk}"
            export LOGOS_LIBLOGOS_ROOT="${logosLiblogos}"
            export LOGOS_PACKAGE_MANAGER_ROOT="${logosPackageManager}"
            export LOGOS_CAPABILITY_MODULE_ROOT="${logosCapabilityModule}"
            export LGX_ROOT="${logosPackageLib}"
            export LOGOS_DESIGN_SYSTEM_ROOT="${logosDesignSystem}"
            
            # Source paths for iOS builds (from flake inputs)
            export LOGOS_CPP_SDK_SRC="${logosCppSdkSrc}"
            export LOGOS_LIBLOGOS_SRC="${logosLiblogosSrc}"
            export LOGOS_PACKAGE_MANAGER_SRC="${logosPackageManagerSrc}"
            export LOGOS_CAPABILITY_MODULE_SRC="${logosCapabilityModuleSrc}"
            
            echo "Logos App development environment"
            echo ""
            echo "Nix packages (host builds):"
            echo "  LOGOS_CPP_SDK_ROOT: $LOGOS_CPP_SDK_ROOT"
            echo "  LOGOS_LIBLOGOS_ROOT: $LOGOS_LIBLOGOS_ROOT"
            echo "  LOGOS_PACKAGE_MANAGER_ROOT: $LOGOS_PACKAGE_MANAGER_ROOT"
            echo "  LOGOS_CAPABILITY_MODULE_ROOT: $LOGOS_CAPABILITY_MODULE_ROOT"
            echo "  LGX_ROOT: $LGX_ROOT"
            echo "  LOGOS_DESIGN_SYSTEM_ROOT: $LOGOS_DESIGN_SYSTEM_ROOT"
            echo ""
            echo "Source paths (for iOS builds):"
            echo "  LOGOS_CPP_SDK_SRC: $LOGOS_CPP_SDK_SRC"
            echo "  LOGOS_LIBLOGOS_SRC: $LOGOS_LIBLOGOS_SRC"
            echo "  LOGOS_PACKAGE_MANAGER_SRC: $LOGOS_PACKAGE_MANAGER_SRC"
            echo "  LOGOS_CAPABILITY_MODULE_SRC: $LOGOS_CAPABILITY_MODULE_SRC"
          '';
        };
      });
    };
}
