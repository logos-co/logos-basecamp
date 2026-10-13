# `logos-basecamp-dev`: standalone mode with dependencies resolved for you.
# logos-module-builder's `nix run .` / `.#ui-dev` run this (`nix run .#dev`).
#
#   --module <install root>   the app (as for LogosBasecamp; repeatable)
#   --dep-package <name>=<dir>  a dependency ready-built as an .lgx package
#                             (repeatable): installed as it is. logos-module-
#                             builder passes every dependency this way, built
#                             by the app's own flake (its follows and
#                             --override-input applied).
#   --dep <name>=<flake ref>  a dependency's source (repeatable). An --app is
#                             built against it; without a --dep-package of
#                             the same name it is also built from it, and its
#                             own dependencies resolved through its lock.
#   --app <name>=<flake ref>  another app to open next to it, built from source
#                             against the --dep revisions it shares
#   --test <file.mjs>         run this logos-qt-mcp test file instead of opening
#                             a window (repeatable): headless, a fresh user dir
#                             per file, as mkPluginTest does; exits non-zero on
#                             the first failure. logos-module-builder's
#                             `nix run .#ui-tests` passes the module's tests/*.mjs.
#
# Everything else goes to LogosBasecamp. The dependencies are installed by
# devDeps.nix; the version check is standalone mode's own.
{ pkgs, system, standaloneApp, logosQtMcp, lgpm }:

let
  launch = pkgs.writeShellScript "logos-basecamp-dev-launch" ''
    mapfile -t args < "$BASECAMP_DEV_ARGS"
    exec ${standaloneApp}/bin/LogosBasecamp "''${args[@]}" --user-dir "$BASECAMP_TEST_USER_DIR" "$@"
  '';
in
pkgs.writeShellApplication {
  name = "logos-basecamp-dev";
  runtimeInputs = [ pkgs.jq pkgs.coreutils ];
  text = ''
    modules=()
    deps=()
    packages=()
    apps=()
    tests=()
    pass=()
    while [ $# -gt 0 ]; do
      case "$1" in
        --module|--dep|--dep-package|--app|--test)
          if [ $# -lt 2 ]; then echo "$1 needs a value" >&2; exit 1; fi
          case "$1" in
            --module) modules+=("$2") ;;
            --dep) deps+=("$2") ;;
            --dep-package) packages+=("$2") ;;
            --app) apps+=("$2") ;;
            --test) tests+=("$2") ;;
          esac
          shift 2 ;;
        --module=*) modules+=("''${1#--module=}"); shift ;;
        --dep=*) deps+=("''${1#--dep=}"); shift ;;
        --dep-package=*) packages+=("''${1#--dep-package=}"); shift ;;
        --app=*) apps+=("''${1#--app=}"); shift ;;
        --test=*) tests+=("''${1#--test=}"); shift ;;
        *) pass+=("$1"); shift ;;
      esac
    done

    provided=()
    for m in "''${modules[@]}"; do
      for d in "$m"/modules/* "$m"/plugins/*; do
        [ -e "$d" ] && provided+=("$(basename "$d")")
      done
    done

    packageEntries=""
    for p in "''${packages[@]}"; do
      packageEntries+="{ name = \"''${p%%=*}\"; package = builtins.storePath \"''${p#*=}\"; } "
    done

    depEntries=""
    for d in "''${deps[@]}"; do
      depEntries+="{ name = \"''${d%%=*}\"; flake = builtins.getFlake \"''${d#*=}\"; } "
    done

    appFlakes=""
    for spec in "''${apps[@]}"; do
      case "$spec" in
        *=*) ;;
        *)
          echo "--app $spec: a bare name (latest release from the catalogue) is not supported yet;" >&2
          echo "  give a flake ref: --app $spec=github:<org>/<repo> or --app $spec=path:<dir>" >&2
          exit 1 ;;
      esac
      appName="''${spec%%=*}"
      ref="''${spec#*=}"
      echo "logos-basecamp-dev: building $appName from $ref" >&2

      # Lock the ref once, so the app and its dependencies come from the same
      # revision.
      meta=$(nix flake metadata "$ref" --json --no-write-lock-file)
      locked=$(jq -r '.url' <<<"$meta")
      inputs=$(jq -r '.locks.nodes[.locks.root].inputs // {} | keys[]' <<<"$meta")

      # Built against the revision of each dependency it shares with the app,
      # which is the copy it will run against.
      overrides=()
      for d in "''${deps[@]}"; do
        if grep -qx "''${d%%=*}" <<<"$inputs"; then
          overrides+=(--override-input "''${d%%=*}" "''${d#*=}")
        fi
      done

      install=$(nix build "$locked#install" "''${overrides[@]}" \
        --no-link --print-out-paths --no-write-lock-file)
      if [ ! -e "$install/plugins/$appName" ] && [ ! -e "$install/modules/$appName" ]; then
        echo "--app $spec: $ref#install has no module named $appName" >&2
        exit 1
      fi
      modules+=("$install")
      provided+=("$appName")
      appFlakes+="(builtins.getFlake \"$locked\") "
    done

    args=()
    for m in "''${modules[@]}"; do args+=(--module "$m"); done

    if [ -n "$packageEntries$depEntries$appFlakes" ]; then
      exclude=""
      for n in "''${provided[@]}"; do exclude+="\"$n\" "; done
      root=$(nix build --impure --no-link --print-out-paths --no-write-lock-file --expr "
        import ${./devDeps.nix} {
          pkgs = import ${pkgs.path} { system = \"${system}\"; };
          system = \"${system}\";
          lgpm = builtins.storePath \"${lgpm}\";
        } {
          packages = [ $packageEntries ];
          deps = [ $depEntries ];
          apps = [ $appFlakes ];
          exclude = [ $exclude ];
        }")
      args+=(--modules-dir "$root")
    fi

    if [ "''${#tests[@]}" -eq 0 ]; then
      exec ${standaloneApp}/bin/LogosBasecamp "''${args[@]}" "''${pass[@]}"
    fi

    # Test mode, as nix/mkPluginTest.nix runs a file — outside the sandbox.
    # The launcher is a store script reading its arguments from a file: one
    # written to the temp dir could not run where /tmp is noexec.
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    export BASECAMP_DEV_ARGS="$work/args"
    printf '%s\n' "''${args[@]}" "''${pass[@]}" > "$BASECAMP_DEV_ARGS"

    export QT_QPA_PLATFORM=offscreen
    export QT_FORCE_STDERR_LOGGING=1
    export QT_LOGGING_RULES="qt.*.debug=false;default.debug=true"
    export LOGOS_QT_MCP="${logosQtMcp}"

    i=0
    for t in "''${tests[@]}"; do
      i=$((i + 1))
      echo "Running: $(basename "$t")"
      export BASECAMP_TEST_USER_DIR="$work/user-dir-$i"
      mkdir -p "$BASECAMP_TEST_USER_DIR"
      timeout "''${LOGOS_TEST_TIMEOUT:-120}" ${pkgs.nodejs}/bin/node "$t" --ci ${launch} --verbose
    done
    echo "All $i test file(s) passed"
  '';
}
