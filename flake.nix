{
  description = "Logos Basecamp - Qt application with UI plugins";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    # Follow the same nixpkgs as logos-nix
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-protocol.url = "github:logos-co/logos-protocol";
    logos-plugin-qt.url = "github:logos-co/logos-plugin-qt";
    logos-qt-sdk.url = "github:logos-co/logos-qt-sdk";
    logos-module.url = "github:logos-co/logos-module";
    logos-module-loader-qt.url = "github:logos-co/logos-module-loader-qt";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-package-manager.url = "github:logos-co/logos-package-manager";
    logos-package-manager-module.url = "github:logos-co/logos-package-manager-module";
    logos-package-downloader-module.url = "github:logos-co/logos-package-downloader-module";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    logos-modules-state-module.url = "github:logos-co/logos-modules-state-module";
    logos-package.url = "github:logos-co/logos-package";
    logos-package-manager-ui.url = "github:logos-co/logos-package-manager-ui";
    logos-design-system.url = "github:logos-co/logos-design-system";
    logos-view-module-runtime.url = "github:logos-co/logos-view-module-runtime";
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

  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU=" ];
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-plugin-qt, logos-qt-sdk, logos-module, logos-module-loader-qt, logos-liblogos, logos-package-manager, logos-package-manager-module, logos-package-downloader-module, logos-capability-module, logos-modules-state-module, logos-package, logos-package-manager-ui, logos-design-system, logos-view-module-runtime, logos-qt-mcp, nix-bundle-logos-module-install, nix-bundle-dir, nix-bundle-appimage, nix-bundle-macos-app }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # Build info (version + commit hashes) baked into the app binary so
      # the Dashboard can render it. Commits come from the flake inputs'
      # locked revs; self's rev is "dirty" when the checkout has uncommitted
      # changes or is overridden via a path input.
      revOf = input: input.rev or input.dirtyRev or "dirty";
      buildInfo = {
        # VERSION is only present on release branches. On master (pre-release
        # CI builds) there is no VERSION file, so fall back to a
        # "pre-release-{sha7}" string derived from self.rev — available on
        # every clean CI checkout. Dirty local builds lack self.rev and get
        # an empty string, which hides the badge (intentional for dev).
        version = if builtins.pathExists ./VERSION
          then nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION)
          else if (self ? rev) then "pre-release-${builtins.substring 0 7 self.rev}" else "";
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
          { name = "logos-modules-state-module"; commit = revOf logos-modules-state-module; }
          { name = "logos-package"; commit = revOf logos-package; }
          { name = "logos-package-manager-ui"; commit = revOf logos-package-manager-ui; }
          { name = "logos-design-system"; commit = revOf logos-design-system; }
          { name = "logos-view-module-runtime"; commit = revOf logos-view-module-runtime; }
          { name = "logos-qt-mcp"; commit = revOf logos-qt-mcp; }
          { name = "nix-bundle-logos-module-install"; commit = revOf nix-bundle-logos-module-install; }
          { name = "nix-bundle-dir"; commit = revOf nix-bundle-dir; }
          { name = "nix-bundle-appimage"; commit = revOf nix-bundle-appimage; }
          { name = "nix-bundle-macos-app"; commit = revOf nix-bundle-macos-app; }
        ];
      };
      # The BUILD platform for a given target. Bundlers and code generators RUN
      # during the build, so on the x86_64-windows cross target they must come
      # from the build system -- taking them from packages.x86_64-windows would
      # hand the builder a PE it cannot execute.
      buildSystemFor = target:
        if target == "x86_64-windows" then "x86_64-linux" else target;

      # forAllSystems, plus the "x86_64-windows" pseudo-system. A cross
      # derivation's `system` attr is its BUILD platform, so the Windows
      # attributes evaluate anywhere and realise on x86_64-linux. Keying it as a
      # system rather than a package-name suffix is what lets the 34
      # `dep.packages.${system}.x` interpolations below stay untouched.
      forAllSystems = f: logos-nix.lib.forAllTargets ({ system, pkgs }:
        let buildSystem = buildSystemFor system; in f {
        inherit system pkgs;
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        # The SAME output carries BOTH the target headers/CMake package AND the
        # logos-cpp-generator binary, so this is a SPLIT, not a swap: keep
        # logosSdk for -DLOGOS_CPP_SDK_ROOT and use logosSdkBuild wherever the
        # generator must RUN. Getting it backwards succeeds on native and, under
        # cross, puts a PE on the builder's PATH -- the symptom is
        # "logos-cpp-generator: command not found".
        logosSdkBuild = logos-cpp-sdk.packages.${buildSystem}.default;
        logosProtocolPkg = logos-protocol.packages.${system}.default;
        logosQtHost = logos-plugin-qt.packages.${system}.logos-qt-host;
        # HEADERS ONLY -- the Qt<->lp seam headers the generated dependency
        # wrapper in app/generated/ includes. Nothing links this.
        logosQtSdk = logos-qt-sdk.packages.${system}.default;
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
        logosModulesStateModule = logos-modules-state-module.packages.${system}.default;
        logosPackageLib = logos-package.packages.${system}.lib;
        # Headers-only output (include/ with logos/semver.hpp + semver/, no
        # library). The app's AppsModel includes the shared semver comparator;
        # it links nothing from lgx, so the headers output keeps liblgx out of
        # the app entirely.
        logosPackageHeaders = logos-package.packages.${system}.headers;
        logosPackageManagerUI = logos-package-manager-ui.packages.${system}.default;
        logosDesignSystem = logos-design-system.packages.${system}.default;
        logosViewModuleRuntime = logos-view-module-runtime.packages.${system}.default;
        # logos-qt-mcp is the QML inspector used by the UI test harness. It has
        # no Windows target and is not needed to RUN the app -- nix/app.nix
        # already takes `logosQtMcp ? null` and gates the inspector on it -- so
        # Windows builds simply go without it. The inspector-dependent outputs
        # (integration-test, shutdown-test, mcp-server) are correspondingly
        # absent from the Windows package set; see the `packages` block.
        logosQtMcp =
          if system == "x86_64-windows" then null
          else logos-qt-mcp.packages.${system}.default;
        logosCppSdkSrc = logos-cpp-sdk.outPath;
        logosLiblogosSrc = logos-liblogos.outPath;
        logosPackageManagerModuleSrc = logos-package-manager-module.outPath;
        logosCapabilityModuleSrc = logos-capability-module.outPath;
        # Bundlers run ON the builder, so they are keyed by buildSystem, not by
        # the target. nix-bundle-dir in particular is ELF/Mach-O only (its
        # bundle.sh branches `file -b` -> Mach-O | ELF with no PE case), so on
        # Windows it must not be invoked at all -- see nix/app.nix.
        # Keyed by the TARGET, not buildSystem: the install bundler now does its
        # own host/target split internally -- it takes lgpm from the build
        # system (it runs there) and the .lgx bundler from the target (which
        # decides the variant name and library extension). Keying the whole
        # thing by buildSystem made it label a Windows package "linux-amd64"
        # and look for a .so payload that was really a .dll.
        installDev = nix-bundle-logos-module-install.bundlers.${system}.dev;
        installPortable = nix-bundle-logos-module-install.bundlers.${system}.portable;
        dirBundler = nix-bundle-dir.bundlers.${buildSystem}.qtApp;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, logosSdk, logosSdkBuild, logosProtocolPkg, logosQtHost, logosQtSdk, logosModule, logosLiblogos, logosLiblogosPortable, logosPackageManagerLibrary, logosPackageManagerModule, logosPackageManagerModuleLib, logosPackageManagerModuleLibPortable, logosPackageDownloaderModule, logosPackageDownloaderModuleLib, logosPackageLib, logosPackageHeaders, logosPackageManagerUI, logosCapabilityModule, logosModulesStateModule, logosDesignSystem, logosViewModuleRuntime, logosQtMcp, installDev, installPortable, dirBundler, ... }:
        let
          # Common configuration
          common = import ./nix/default.nix {
            inherit pkgs logosSdk logosProtocolPkg logosQtHost logosModule logosLiblogos;
          };
          src = ./.;

          # Basecamp's own UI shell: a privilege-free plugin that links Qt and
          # nothing else from this workspace, which nix/symbol-gate.nix enforces
          # across the in-process image set. It builds from the SAME `src` as
          # the app; its CMakeLists lives in src/ and can only see
          # app/interfaces/, so it cannot include a host header even by accident.
          mainUIPlugin = import ./nix/main-ui.nix {
            inherit pkgs common src logosDesignSystem;
          };

          packageManagerUIPlugin = logosPackageManagerUI;

          # Pre-installed modules/plugins (bundle + lgpm install in one step).
          # Dev build: raw derivation (depends on /nix/store at runtime).
          # Distributed build: portable self-contained bundle. Off Windows that
          # portability comes from nix-bundle-dir; on Windows nix-bundle-lgx
          # takes its own `mkWindowsPayload` path instead and does not call
          # nix-bundle-dir at all. That is nix-bundle-lgx's business, not this
          # flake's -- a MODULE must not carry the Qt/OpenSSL/runtime DLLs the
          # host already ships in bin/, so the PE path needs a hostLibs strip
          # that nix-bundle-dir does not have yet. Unrelated to binBundleDir
          # below, which is the APP and therefore is the thing that ships them.
          installedDev = map installDev [
            logosPackageManagerModuleLib
            logosPackageDownloaderModuleLib
            logosCapabilityModule
            # The module lifecycle registry. liblogos feeds it load/unload/crash
            # as sequenced facts, which is what lets a consumer stop polling --
            # PackageCoordinator's settle-timer inference is what this replaces.
            # Optional by construction: absent, the feed never arms.
            logosModulesStateModule
            packageManagerUIPlugin
          ];
          installedDistributed = map installPortable [
            logosPackageManagerModuleLibPortable
            logosPackageDownloaderModuleLib
            logosCapabilityModule
            logosModulesStateModule
            packageManagerUIPlugin
          ];

          # App package (development build)
          app = import ./nix/app.nix {
            inherit pkgs common src logosModule logosLiblogos logosSdk logosProtocolPkg logosQtHost logosQtSdk logosDesignSystem logosViewModuleRuntime logosPackageManagerModule logosPackageDownloaderModule logosPackageHeaders buildInfo logosSdkBuild;
            inherit logosQtMcp mainUIPlugin;
            installedModules = installedDev;
          };

          # App package (distributed build for DMG/AppImage)
          # Uses portable-compiled liblogos for portable variant selection
          appDistributed = import ./nix/app.nix {
            inherit pkgs common src logosModule logosSdk logosProtocolPkg logosQtHost logosQtSdk logosDesignSystem logosViewModuleRuntime logosPackageManagerModule logosPackageDownloaderModule logosPackageHeaders buildInfo logosSdkBuild;
            inherit mainUIPlugin;
            logosLiblogos = logosLiblogosPortable;
            installedModules = installedDistributed;
            portable = true;
            enableInspector = false;
          };

          # Distributed build with inspector enabled (for macOS integration tests)
          appDistributedWithInspector = import ./nix/app.nix {
            inherit pkgs common src logosModule logosSdk logosProtocolPkg logosQtHost logosQtSdk logosDesignSystem logosViewModuleRuntime logosPackageManagerModule logosPackageDownloaderModule logosPackageHeaders buildInfo logosSdkBuild;
            inherit logosQtMcp mainUIPlugin;
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

          # (There is no ./nix/appimage.nix binding here. The shipped AppImage is
          # the `bin-appimage` output further down, built by nix-bundle-appimage.
          # A dead `appImage = import ./nix/appimage.nix ...` binding survived
          # here long after that file was removed, evaluating only because
          # nothing ever forced it.)

          # Self-contained directory bundle: appDistributed modules expect host Qt
          # via @rpath; qtApp copies Qt frameworks into lib/ and rewrites the binary.
          # (appDistributed alone is an intermediate used by AppImage / .app wrappers.)
          withMainProgram = drv: drv.overrideAttrs (old: {
            meta = (old.meta or {}) // {
              mainProgram = "LogosBasecamp";
            };
          });
          # dirBundler on EVERY platform, Windows included.
          #
          # The `winBundler = drv: drv` / `bundleFor` bypass that used to sit
          # here was placed on an explicit condition: "the real fix is a PE
          # branch in nix-bundle-dir's bundle.sh that skips relocation and keeps
          # Qt staging; when that lands, DELETE bundleFor". It has landed, and
          # this flake's root `nix-bundle-dir` input already resolves to it
          # (f843b8ec, which is `main`), so the condition is met and the bypass
          # is gone.
          #
          # Why it was never a working alternative: nix-bundle-dir does two
          # separable jobs. (a) RELOCATION -- rewriting rpaths / install names
          # so binaries stop pointing into /nix/store. (b) Qt STAGING -- the Qt
          # plugin scan, the QML module scan and qt.conf generation; that half
          # is FORMAT-AGNOSTIC and Windows needs it exactly as much as anywhere
          # else, because Qt plugins and QML module DLLs are LoadLibrary'd and
          # nothing in the import table reveals them.
          #
          # The bypass was argued for on the grounds that a PE needs none of
          # (a). Only the rpath REWRITING half of that is true, and the
          # measurement below is what corrects it: the un-bundled tree reaches
          # a third of its bin/ through 12 SYMLINKS into /nix/store, which no
          # amount of "PE imports are base names" makes portable. Phase 1's
          # `cp -aL` is what dereferences them, and skipping the bundler
          # skipped that as surely as it skipped (b).
          #
          # Measured, not assumed. Both trees were realised on x86_64-linux
          # from ONE tree -- same appDistributed derivation, the only variable
          # being whether dirBundler is applied -- and compared entry by entry:
          #
          #                              bypass(drv:drv)   dirBundler
          #     entries                              85         1740
          #     regular files                        59         1657
          #     symlinks                             12            0
          #     bytes                            259 MB       479 MB
          #     *.dll in bin/                        33           88
          #     bin/qt.conf                     MISSING      present
          #     lib/qt-6/…/platforms/qwindows.dll
          #                                     MISSING      present
          #     lib/qt-6 (plugins + qml)        MISSING   1533 files
          #     nix closure refs                     12            1
          #
          # Those 12 symlinks are the part that matters most, and they are why
          # "the bypass at least shipped the app" was never true. bin/Qt6Core
          # .dll, Qt6Gui.dll, Qt6Widgets.dll, Qt6Network.dll, Qt6RemoteObjects
          # .dll, libssl/libcrypto, libpng16, libzstd, libb2, pcre2 and
          # double-conversion were SYMLINKS into /nix/store. Copy that tree to
          # a Windows box -- the entire point of a portable bundle -- and every
          # one of them dangles: 0xC0000135, no output, before main(). The
          # bundled tree resolves all 12 into real files and has no symlink
          # left. Its single remaining nix reference is an inert /nix string
          # embedded in a PE's data, which imports nothing.
          #
          # 55 DLLs exist only in the bundled bin/: the Qt Quick / Controls /
          # Labs set the fixpoint sweep pulls in once the QML modules are
          # staged, plus libcurl and its TLS/HTTP2 chain mirrored beside the
          # package_downloader module that imports them.
          #
          # The one thing the bundler does NOT carry over: README.txt and
          # share/ (a .desktop file and a hicolor icon, 9 paths). That is not a
          # Windows regression -- bundle.sh Phase 1 copies bin/, lib/ and
          # extraDirs on EVERY platform, so the shipping Linux and macOS
          # bundles have never had them either; the bypass "kept" them only by
          # doing nothing at all. Both are dead weight off-store anyway:
          # README.txt is a build-info file whose every line is a /nix/store
          # path, and a .desktop file does nothing on Windows.
          #
          # Getting here also required three additions to the app's
          # passthru.extraClosurePaths (see nix/app.nix) -- qtdeclarative,
          # libjpeg.bin, sqlite.bin. Each was a build the bundler FAILED,
          # naming the missing DLL and the plugin that imported it, rather than
          # shipping a tree that dies before main(). That is the behaviour the
          # bypass was hiding.
          #
          # NOT demonstrated, stated plainly:
          #
          #  * None of this has been RUN on Windows, by this change or by CI,
          #    which has never executed a Windows binary. Every claim above is
          #    build-time and tree-shape only.
          #  * The two trees measured came from a harness that dropped ONE
          #    entry from installedDistributed -- packageManagerUIPlugin --
          #    because logos-package-manager-ui did not cross-compile at the rev
          #    this flake pinned then: its generated logos_sdk.h included
          #    package_manager_api.h, which was not produced for the Windows
          #    target. There was also a separate EVAL-time blocker, a
          #    logos-package-downloader-module with no x86_64-windows target.
          #
          #    BOTH have since been fixed upstream. `.#packages.x86_64-windows.*`
          #    now evaluates AND builds, and the resulting bundle DOES include
          #    package_manager_ui -- so the numbers above describe a tree that
          #    was missing a plugin which is no longer missing. Re-measure
          #    before quoting them.
          #
          #    (The cross build needs an x86_64-linux builder: logos_build_info.h
          #    is an x86_64-linux derivation, so it cannot run on an
          #    aarch64-darwin host without one.)
          binBundleDir = withMainProgram (dirBundler appDistributed);
          binBundleDirInspector = withMainProgram (dirBundler appDistributedWithInspector);

          # Hoisted so shutdown-test can read the elapsed time for the combined PR-gate budget.
          integrationTest = import ./nix/integration-test.nix { inherit pkgs src logosQtMcp; appPkg = app; };
          integrationTestBundle = import ./nix/integration-test.nix {
            inherit pkgs src;
            appPkg = macosAppTest;
            inherit logosQtMcp;
            appBin = "${macosAppTest}/LogosBasecamp.app/Contents/MacOS/LogosBasecamp";
          };
        in
        {
          # Individual outputs.
          main-ui-plugin = mainUIPlugin;
          package-manager-ui-plugin = packageManagerUIPlugin;
          app = app;

          # Self-contained flat directory (bin/ + lib/ with Qt).
          # Run: nix run .#bin-bundle-dir
          bin-bundle-dir = binBundleDir;

          # Test-only twin of bin-bundle-dir WITH the QML inspector compiled in,
          # so logos-qt-mcp can connect and drive the UI headlessly. Identical to
          # the shipping bundle in every other respect.
          #
          # The inspector is a compile-time feature and is deliberately OFF in the
          # shipping bin-bundle-dir / appimage / macos outputs — we do NOT ship
          # the inspector in release builds. This output exists purely so the
          # package-manager doc-test can install and exercise modules through
          # the real bundled UI; it is not a release artifact.
          # Build: nix build .#bin-bundle-dir-inspector
          bin-bundle-dir-inspector = binBundleDirInspector;

          # QML Inspector MCP server: nix build .#mcp-server -o result-mcp
          mcp-server = logos-qt-mcp.packages.${system}.mcp-server;

          # Full logos-qt-mcp package (includes test-framework, mcp-server, qt-plugin)
          # Use: nix build .#logos-qt-mcp -o result-mcp
          # Then: LOGOS_QT_MCP=./result-mcp node tests/ui-tests.mjs --ci ./result/bin/LogosBasecamp
          logos-qt-mcp = logosQtMcp;

          # Smoke test (also exposed as a package so it can be built standalone)
          smoke-test = import ./nix/smoke-test.nix { inherit pkgs; appPkg = app; };

          # One-runtime symbol gate. Asserts the logos C++ runtime (TokenManager,
          # StoreRegistry, LogosAPI, LogosAPIClient) is DEFINED exactly once across
          # the images that share one process. The assertion is exactly-one and
          # deliberately does NOT name an owner: liblogos_core stopped being the
          # provider when the runtime became real shared libraries, and it is
          # liblogos_protocol and liblogos_qt_host that define these types today.
          # A second definition is a second TokenManager and every cross-module
          # call is refused at runtime with no build diagnostic.
          # Build: nix build .#symbol-gate  (CI: the "One-runtime symbol gate"
          # step in test-linux, test-macos AND build-windows runs this and its
          # negative control -- Windows being the one where a duplicate is fatal,
          # since PE has no symbol interposition to collapse it)
          symbol-gate = import ./nix/symbol-gate.nix { inherit pkgs; appPkg = app; };

          # Negative control for the above. Plants a REAL duplicate runtime where
          # an in-process consumer goes and asserts the gate REJECTS it. Ship both
          # or neither: an absence assertion that has never been seen to fail is
          # indistinguishable from a broken one.
          # Build: nix build .#symbol-gate-negative
          symbol-gate-negative = import ./nix/symbol-gate.nix {
            inherit pkgs; appPkg = app; negativeControl = true;
          };

          # ui_qml sandbox-escape regression test (F-008). Focused C++ unit test:
          # builds a real malicious QML plugin and asserts the production sandbox
          # refuses to load it. Build: nix build .#sandbox-test
          sandbox-test = import ./nix/sandbox-test.nix { inherit pkgs src; };

          # Pure-model unit tests (AppsModel install-status logic, etc.). Same
          # shape as sandbox-test — standalone QtTest project, no app launch,
          # no IPC. Build: nix build .#unit-tests
          unit-tests = import ./nix/unit-tests.nix {
            inherit pkgs src logosPackageHeaders;
          };

          # QML component tests (Qt Quick Test)
          qml-tests = import ./nix/qml-tests.nix { inherit pkgs src logosPackageHeaders; };

          # Coverage report for the unit-test suite: same targets as
          # .#unit-tests, compiled with --coverage and reported via gcovr.
          # Report-only for now (failUnderLine = 0) — raise the threshold as
          # the test plan phases land to make it a gate.
          # Build: nix build .#coverage -L && open result/coverage.html
          coverage = import ./nix/coverage.nix {
            inherit pkgs src logosPackageHeaders;
            failUnderLine = 0;
          };

          # Integration test (UI tests via Qt Inspector)
          integration-test = integrationTest;

          # Host-services grant guard. Asserts that a NON-"core" identity
          # (ui-host running package_manager_ui) actually completes a
          # capability-gated call chain — i.e. that capability_module really
          # received its token_registry/token_delivery grant from the loader
          # basecamp pins, rather than failing closed. See
          # nix/host-services-test.nix and tests/host-services-assert.mjs.
          # Build: nix build .#host-services-test
          host-services-test = import ./nix/host-services-test.nix {
            inherit pkgs src logosQtMcp; appPkg = app;
          };

          # Shutdown tests (SIGTERM, SIGINT, Ctrl+Q / ⌘Q). Spawns a fresh
          # app per case and asserts orderly exit (code 0).
          shutdown-test = import ./nix/shutdown-test.nix {
            inherit pkgs src logosQtMcp;
            appPkg = app;
            uiTestRun = if pkgs.stdenv.isDarwin then integrationTestBundle else integrationTest;
          };

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
          host-services-test-bundle = import ./nix/host-services-test.nix {
            inherit pkgs src;
            appPkg = macosAppTest;
            inherit logosQtMcp;
            appBin = "${macosAppTest}/LogosBasecamp.app/Contents/MacOS/LogosBasecamp";
          };
        }
      );

      # nix run .                   → dev build  (depends on /nix/store at runtime)
      # nix run .#bin-bundle-dir    → self-contained bundle (Qt frameworks in lib/)
      apps = forAllSystems ({ system, ... }: {
        default = {
          type = "app";
          program = "${self.packages.${system}.app}/bin/LogosBasecamp";
        };
        bin-bundle-dir = {
          type = "app";
          program = "${self.packages.${system}.bin-bundle-dir}/bin/LogosBasecamp";
        };
      });

      checks = forAllSystems ({ pkgs, system, ... }: {
        smoke-test = self.packages.${system}.smoke-test;
        sandbox-test = self.packages.${system}.sandbox-test;
        unit-tests = self.packages.${system}.unit-tests;
        qml-tests = self.packages.${system}.qml-tests;
        integration-test = self.packages.${system}.integration-test;
        shutdown-test = self.packages.${system}.shutdown-test;
        host-services-test = self.packages.${system}.host-services-test;
        symbol-gate = self.packages.${system}.symbol-gate;
        symbol-gate-negative = self.packages.${system}.symbol-gate-negative;
      });

      devShells = forAllSystems ({ pkgs, logosSdk, logosProtocolPkg, logosQtHost, logosModule, logosLiblogos, logosPackageManagerLibrary, logosPackageManagerModule, logosCapabilityModule, logosPackageLib, logosDesignSystem, logosCppSdkSrc, logosLiblogosSrc, logosPackageManagerModuleSrc, logosCapabilityModuleSrc, ... }: {
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
            export LOGOS_PROTOCOL_ROOT="${logosProtocolPkg}"
            export LOGOS_QT_HOST_ROOT="${logosQtHost}"
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
            
            echo "Logos Basecamp development environment"
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
