# Lifecycle tests for logos-basecamp: tests/lifecycle-tests.mjs, the nightly
# suite of the MCP test plan (install gates, failure paths, uninstall
# cascades, module IPC, restart). A fresh app and user dir per test; the
# runner captures each app's output itself, so there is no log wrapper and
# nothing is pre-seeded here. Nightly, so no PR-gate budget applies.
#
# Requires Node.js, the Qt offscreen platform, the MCP inspector and the lgx
# CLI (the fixtures are built at run time).
{ pkgs, src, appPkg, logosQtMcp, lgx
# Appended to the platform variant fixtures are built for: a dev build
# accepts only "<platform>-dev", a portable bundle only "<platform>".
, fixtureVariantSuffix ? "-dev"
, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 900 }:

pkgs.runCommand "logos-basecamp-lifecycle-test" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.nodejs ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
      pkgs.qt6.qtbase   # provides the offscreen platform plugin
      pkgs.libGL
      pkgs.libglvnd
    ];
} ''
  mkdir -p $out
  export HOME="$TMPDIR/home"
  mkdir -p "$HOME"

  # The fixtures are built the way lgpm packages one, content hashes
  # included: Basecamp refuses a user-installed package without them.
  export LGX_CLI="${lgx}/bin/lgx"
  export LGX_VARIANT="$(${pkgs.nodejs}/bin/node --input-type=module -e "
    import { platformVariant } from '${src}/tests/fixtures/lgx.mjs';
    console.log(platformVariant());
  ")${fixtureVariantSuffix}"

  # Never take over the machine's basecamp:// handler from a test.
  export LOGOS_NO_SCHEME_REGISTER=1

  export QT_QPA_PLATFORM=offscreen
  export QT_FORCE_STDERR_LOGGING=1
  export QT_LOGGING_RULES="qt.*.debug=false;default.debug=true"

  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    export LD_LIBRARY_PATH="${pkgs.libGL}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ''}

  export LOGOS_QT_MCP="${logosQtMcp}"

  set -o pipefail
  echo "Running logos-basecamp lifecycle tests (timeout: ${toString timeoutSec}s)..."

  start=$(date +%s)
  timeout ${toString timeoutSec} \
    ${pkgs.nodejs}/bin/node ${src}/tests/lifecycle-tests.mjs ${appBin} | tee $out/results.txt
  elapsed=$(( $(date +%s) - start ))
  echo "$elapsed" > $out/elapsed-seconds

  echo "Lifecycle tests passed in ''${elapsed}s"
''
