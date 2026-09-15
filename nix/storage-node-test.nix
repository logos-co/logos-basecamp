# Launches the app with the real storage_module and checks that StorageNode
# starts the node, then stops it on SIGTERM.
{ pkgs, appPkg, appBin ? "${appPkg}/bin/LogosBasecamp", timeoutSec ? 60 }:

pkgs.runCommand "logos-basecamp-storage-node-test" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.gnugrep ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
      pkgs.qt6.qtbase   # provides the offscreen platform plugin
      pkgs.libGL
      pkgs.libglvnd
    ];
} ''

  mkdir -p $out
  export LOGOS_USER_DIR="$out/app-data"
  mkdir -p "$LOGOS_USER_DIR"

  # Never take over the machine's basecamp:// handler from a test.
  export LOGOS_NO_SCHEME_REGISTER=1

  # The node keeps its data under $HOME/.logos_storage.
  export HOME="$TMPDIR/home"
  mkdir -p "$HOME"

  export QT_QPA_PLATFORM=offscreen
  export QT_FORCE_STDERR_LOGGING=1
  export QT_LOGGING_RULES="qt.*.debug=false;default.debug=true"

  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
    export LD_LIBRARY_PATH="${pkgs.libGL}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ''}

  LOG="$out/storage-node-test.log"

  ${appBin} -platform offscreen > "$LOG" 2>&1 &
  APP=$!

  for _ in $(seq ${toString timeoutSec}); do
    grep -q "Storage node started." "$LOG" && break
    sleep 1
  done

  if ! grep -q "Storage node started." "$LOG"; then
    kill -KILL "$APP" 2>/dev/null || true
    cat "$LOG"
    echo "FAIL: the storage node did not start within ${toString timeoutSec}s"
    exit 1
  fi

  kill -TERM "$APP"
  wait "$APP" || true

  cat "$LOG"

  if ! grep -q "Storage node stopped." "$LOG"; then
    echo "FAIL: the storage node did not stop on SIGTERM"
    exit 1
  fi

  echo "Storage node test passed"
''
