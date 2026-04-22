{
  description = "Logos App - Qt application with UI plugins";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    # Follow the same nixpkgs as logos-nix
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-module.url = "github:logos-co/logos-module";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-package-manager.url = "github:logos-co/logos-package-manager";
    logos-package-manager-module.url = "github:logos-co/logos-package-manager-module";
    logos-package-downloader-module.url = "github:logos-co/logos-package-downloader-module";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    logos-package.url = "github:logos-co/logos-package";
    logos-package-manager-ui.url = "github:logos-co/logos-package-manager-ui";
    logos-webview-app.url = "github:logos-co/logos-webview-app";
    logos-design-system.url = "github:logos-co/logos-design-system";
    logos-counter-qml.url = "github:logos-co/counter_qml";
    logos-counter.url = "github:logos-co/counter";
    logos-view-module-runtime = {
      url = "github:logos-co/logos-view-module-runtime";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    };
    nix-bundle-logos-module-install.url = "github:logos-co/nix-bundle-logos-module-install";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    logos-qt-mcp.url = "github:logos-co/logos-qt-mcp";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
    nix-bundle-macos-app = {
      url = "github:logos-co/nix-bundle-macos-app";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.nix-bundle-dir.follows = "nix-bundle-dir";
    };
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-module, logos-liblogos, logos-package-manager, logos-package-manager-module, logos-package-downloader-module, logos-capability-module, logos-package, logos-package-manager-ui, logos-webview-app, logos-design-system, logos-counter-qml, logos-counter, logos-view-module-runtime, logos-qt-mcp, nix-bundle-logos-module-install, nix-bundle-dir, nix-bundle-appimage, nix-bundle-macos-app }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # Build info (version + commit hashes) baked into the main UI plugin so
      # the Dashboard can render it. Commits come from the flake inputs'
      # locked revs; self's rev is "dirty" when the checkout has uncommitted
      # changes or is overridden via a path input.
      revOf = input: input.rev or input.dirtyRev or "dirty";
      buildInfo = {
        # VERSION is only present on release branches; absent = empty string
        # (Dashboard hides the version line when empty).
        version = if builtins.pathExists ./VERSION
          then nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION)
          else "";
        commits = [
          { name = "logos-basecamp"; commit = revOf self; }
          { name = "logos-nix"; commit = revOf logos-nix; }
          { name = "logos-cpp-sdk"; commit = revOf logos-cpp-sdk; }
          { name = "logos-module"; commit = revOf logos-module; }
          { name = "logos-liblogos"; commit = revOf logos-liblogos; }
          { name = "logos-package-manager"; commit = revOf logos-package-manager; }
          { name = "logos-package-manager-module"; commit = revOf logos-package-manager-module; }
          { name = "logos-package-downloader-module"; commit = revOf logos-package-downloader-module; }
          { name = "logos-capability-module"; commit = revOf logos-capability-module; }
          { name = "logos-package"; commit = revOf logos-package; }
          { name = "logos-package-manager-ui"; commit = revOf logos-package-manager-ui; }
          { name = "logos-webview-app"; commit = revOf logos-webview-app; }
          { name = "logos-design-system"; commit = revOf logos-design-system; }
          { name = "logos-counter-qml"; commit = revOf logos-counter-qml; }
          { name = "logos-counter"; commit = revOf logos-counter; }
          { name = "logos-view-module-runtime"; commit = revOf logos-view-module-runtime; }
          { name = "logos-qt-mcp"; commit = revOf logos-qt-mcp; }
          { name = "nix-bundle-logos-module-install"; commit = revOf nix-bundle-logos-module-install; }
          { name = "nix-bundle-dir"; commit = revOf nix-bundle-dir; }
          { name = "nix-bundle-appimage"; commit = revOf nix-bundle-appimage; }
          { name = "nix-bundle-macos-app"; commit = revOf nix-bundle-macos-app; }
        ];
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosModule = logos-module.packages.${system}.default;
        logosLiblogos = logos-liblogos.packages.${system}.default;
        logosPackageManagerLibrary = logos-package-manager.packages.${system}.lib;
        logosPackageManagerModule = logos-package-manager-module.packages.${system}.default;
        logosPackageManagerModuleLib = logos-package-manager-module.packages.${system}.lib;
        logosPackageDownloaderModule = logos-package-downloader-module.packages.${system}.default;
        logosPackageDownloaderModuleLib = logos-package-downloader-module.packages.${system}.lib;
        logosLiblogosPortable = logos-liblogos.packages.${system}.portable;
        logosPackageManagerModuleLibPortable = logos-package-manager-module.packages.${system}.lib-portable;
        logosCapabilityModule = logos-capability-module.packages.${system}.default;
        logosPackageLib = logos-package.packages.${system}.lib;
        logosPackageManagerUI = logos-package-manager-ui.packages.${system}.default;
        logosWebviewApp = logos-webview-app.packages.${system}.default;
        logosDesignSystem = logos-design-system.packages.${system}.default;
        logosCounterQml = logos-counter-qml.packages.${system}.default;
        logosCounter = logos-counter.packages.${system}.default;
        logosViewModuleRuntime = logos-view-module-runtime.packages.${system}.default;
        logosQtMcp = logos-qt-mcp.packages.${system}.default;
        logosCppSdkSrc = logos-cpp-sdk.outPath;
        logosLiblogosSrc = logos-liblogos.outPath;
        logosPackageManagerModuleSrc = logos-package-manager-module.outPath;
        logosCapabilityModuleSrc = logos-capability-module.outPath;
        installDev = nix-bundle-logos-module-install.bundlers.${system}.dev;
        installPortable = nix-bundle-logos-module-install.bundlers.${system}.portable;
        dirBundler = nix-bundle-dir.bundlers.${system}.qtApp;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, logosSdk, logosModule, logosLiblogos, logosLiblogosPortable, logosPackageManagerLibrary, logosPackageManagerModule, logosPackageManagerModuleLib, logosPackageManagerModuleLibPortable, logosPackageDownloaderModule, logosPackageDownloaderModuleLib, logosPackageLib, logosPackageManagerUI, logosCapabilityModule, logosWebviewApp, logosDesignSystem, logosCounterQml, logosCounter, logosViewModuleRuntime, logosQtMcp, installDev, installPortable, dirBundler, ... }:
        let
          # Common configuration
          common = import ./nix/default.nix {
            inherit pkgs logosSdk logosModule logosLiblogos;
          };
          src = ./.;

          # Plugin packages (development builds)
          counterPlugin = logosCounter;
          counterQmlPlugin = logosCounterQml;
          mainUIPlugin = import ./nix/main-ui.nix {
            inherit pkgs common src logosSdk logosModule logosPackageManagerModule logosLiblogos logosViewModuleRuntime buildInfo;
          };
          packageManagerUIPlugin = logosPackageManagerUI;
          webviewAppPlugin = logosWebviewApp;

          # Plugin packages (distributed builds for DMG/AppImage)
          mainUIPluginDistributed = import ./nix/main-ui.nix {
            inherit pkgs common src logosSdk logosModule logosPackageManagerModule logosLiblogos logosViewModuleRuntime buildInfo;
            distributed = true;
          };

          # Pre-installed modules/plugins (bundle + lgpm install in one step).
          # Dev build: raw derivation (depends on /nix/store at runtime).
          # Distributed build: portable self-contained bundle (nix-bundle-dir pre-applied).
          installedDev = map installDev [
            logosPackageManagerModuleLib
            logosPackageDownloaderModuleLib
            logosCapabilityModule
            counterPlugin
            counterQmlPlugin
            mainUIPlugin
            packageManagerUIPlugin
            webviewAppPlugin
          ];
          installedDistributed = map installPortable [
            logosPackageManagerModuleLibPortable
            logosPackageDownloaderModuleLib
            logosCapabilityModule
            counterPlugin
            counterQmlPlugin
            mainUIPluginDistributed
            packageManagerUIPlugin
            webviewAppPlugin
          ];

          # App package (development build)
          app = import ./nix/app.nix {
            inherit pkgs common src logosModule logosLiblogos logosSdk logosDesignSystem logosViewModuleRuntime buildInfo;
            inherit logosQtMcp;
            installedModules = installedDev;
          };

          # App package (distributed build for DMG/AppImage)
          # Uses portable-compiled liblogos for portable variant selection
          appDistributed = import ./nix/app.nix {
            inherit pkgs common src logosModule logosSdk logosDesignSystem logosViewModuleRuntime buildInfo;
            logosLiblogos = logosLiblogosPortable;
            installedModules = installedDistributed;
            portable = true;
            enableInspector = false;
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

          # Distributed build with inspector enabled (for macOS integration tests)
          appDistributedWithInspector = import ./nix/app.nix {
            inherit pkgs common src logosModule logosSdk logosDesignSystem logosViewModuleRuntime buildInfo;
            inherit logosQtMcp;
            logosLiblogos = logosLiblogosPortable;
            installedModules = installedDistributed;
            portable = true;
            enableInspector = true;
          };

          # macOS app for testing (distributed build with inspector enabled)
          macosAppTest = if pkgs.stdenv.isDarwin then
            nix-bundle-macos-app.lib.${system}.mkMacOSApp {
              drv = appDistributedWithInspector;
              name = "LogosBasecamp";
              bundle = dirBundler appDistributedWithInspector;
              icon = ./app/macos/logos.icns;
              infoPlist = ./app/macos/Info.plist.in;
              entitlements = ./app/macos/LogosBasecamp.entitlements;
            }
          else null;

          macosApp = if pkgs.stdenv.isDarwin then
            nix-bundle-macos-app.lib.${system}.mkMacOSApp {
              drv = appDistributed;
              name = "LogosBasecamp";
              bundle = dirBundler appDistributed;
              icon = ./app/macos/logos.icns;
              infoPlist = ./app/macos/Info.plist.in;
              entitlements = ./app/macos/LogosBasecamp.entitlements;
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
          
          # Bundle outputs
          bin-bundle-dir = dirBundler appDistributed;

          # QML Inspector MCP server: nix build .#mcp-server -o result-mcp
          mcp-server = logos-qt-mcp.packages.${system}.mcp-server;

          # Full logos-qt-mcp package (includes test-framework, mcp-server, qt-plugin)
          # Use: nix build .#logos-qt-mcp -o result-mcp
          # Then: LOGOS_QT_MCP=./result-mcp node tests/ui-tests.mjs --ci ./result/bin/LogosBasecamp
          logos-qt-mcp = logosQtMcp;

          # Smoke test (also exposed as a package so it can be built standalone)
          smoke-test = import ./nix/smoke-test.nix { inherit pkgs; appPkg = app; };

          # Integration test (UI tests via Qt Inspector)
          integration-test = import ./nix/integration-test.nix { inherit pkgs src logosQtMcp; appPkg = app; };

          # Default package
          default = app;
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          bin-appimage = nix-bundle-appimage.lib.${system}.mkAppImage {
            drv = appDistributed;
            name = "logos-basecamp";
            bundle = dirBundler appDistributed;
            desktopFile = ./assets/logos-basecamp.desktop;
            icon = ./app/icons/logos.png;
          };
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isDarwin {
          bin-macos-app = macosApp;
          smoke-test-bundle = import ./nix/smoke-test.nix {
            inherit pkgs;
            appPkg = macosApp;
            appBin = "${macosApp}/LogosBasecamp.app/Contents/MacOS/LogosBasecamp";
          };
          integration-test-bundle = import ./nix/integration-test.nix {
            inherit pkgs src;
            appPkg = macosAppTest;
            inherit logosQtMcp;
            appBin = "${macosAppTest}/LogosBasecamp.app/Contents/MacOS/LogosBasecamp";
          };
        } // (if pkgs.stdenv.isDarwin then {
          # macOS distribution outputs
          app-bundle = appBundle;
          inherit dmg;
        } else {}) // (if pkgs.stdenv.isLinux then {
          # Linux distribution output
          appimage = appImage;
        } else {})
      );

      checks = forAllSystems ({ pkgs, system, ... }: {
        smoke-test = self.packages.${system}.smoke-test;
        integration-test = self.packages.${system}.integration-test;
      });

      devShells = forAllSystems ({ pkgs, logosSdk, logosModule, logosLiblogos, logosPackageManagerLibrary, logosPackageManagerModule, logosCapabilityModule, logosPackageLib, logosDesignSystem, logosCppSdkSrc, logosLiblogosSrc, logosPackageManagerModuleSrc, logosCapabilityModuleSrc }: {
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
            export LOGOS_MODULE_ROOT="${logosModule}"
            export LOGOS_LIBLOGOS_ROOT="${logosLiblogos}"
            export LOGOS_PACKAGE_MANAGER_ROOT="${logosPackageManagerLibrary}"
            export LOGOS_CAPABILITY_MODULE_ROOT="${logosCapabilityModule}"
            export LGX_ROOT="${logosPackageLib}"
            export LOGOS_DESIGN_SYSTEM_ROOT="${logosDesignSystem}"
            
            # Source paths for iOS builds (from flake inputs)
            export LOGOS_CPP_SDK_SRC="${logosCppSdkSrc}"
            export LOGOS_LIBLOGOS_SRC="${logosLiblogosSrc}"
            export LOGOS_PACKAGE_MANAGER_MODULE_SRC="${logosPackageManagerModuleSrc}"
            export LOGOS_CAPABILITY_MODULE_SRC="${logosCapabilityModuleSrc}"
            
            echo "Logos App development environment"
            echo ""
            echo "Nix packages (host builds):"
            echo "  LOGOS_CPP_SDK_ROOT: $LOGOS_CPP_SDK_ROOT"
            echo "  LOGOS_MODULE_ROOT: $LOGOS_MODULE_ROOT"
            echo "  LOGOS_LIBLOGOS_ROOT: $LOGOS_LIBLOGOS_ROOT"
            echo "  LOGOS_PACKAGE_MANAGER_ROOT: $LOGOS_PACKAGE_MANAGER_ROOT"
            echo "  LOGOS_CAPABILITY_MODULE_ROOT: $LOGOS_CAPABILITY_MODULE_ROOT"
            echo "  LGX_ROOT: $LGX_ROOT"
            echo "  LOGOS_DESIGN_SYSTEM_ROOT: $LOGOS_DESIGN_SYSTEM_ROOT"
            echo ""
            echo "Source paths (for iOS builds):"
            echo "  LOGOS_CPP_SDK_SRC: $LOGOS_CPP_SDK_SRC"
            echo "  LOGOS_LIBLOGOS_SRC: $LOGOS_LIBLOGOS_SRC"
            echo "  LOGOS_PACKAGE_MANAGER_MODULE_SRC: $LOGOS_PACKAGE_MANAGER_MODULE_SRC"
            echo "  LOGOS_CAPABILITY_MODULE_SRC: $LOGOS_CAPABILITY_MODULE_SRC"
          '';
        };
      });
    };
}
