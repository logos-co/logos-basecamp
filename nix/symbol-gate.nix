# The one-runtime symbol gate.
#
# INVARIANT: across the images that share ONE process, the logos C++ runtime is
# DEFINED exactly once — in liblogos_core, the single provider. A second
# definition is a second TokenManager: the host writes a capability token into
# one store, another image reads an empty one, and every cross-module call is
# refused at runtime with NO build diagnostic. See
# logos-protocol/cpp/logos_shared_api.h and cmake/LogosSharedFromDll.cmake.
#
# This gate exists because nothing else in the tree measured it. The main_ui
# fold shipped an executable that defined nine runtime entry points
# liblogos_core.dylib did not export, and it was caught by hand, after the fact,
# from 31 "ModuleProxy: rejecting unauthorized call" lines at runtime.
#
# THE IN-PROCESS IMAGE SET — this scoping IS the correctness of the gate:
#   IN   bin/LogosBasecamp                the app
#   IN   lib/liblogos_core.*              the single provider
#   IN   plugins/*/*_replica_factory.*    QPluginLoader'd by LogosQmlBridge
#   IN   plugins/main_ui/main_ui.*        QPluginLoader'd by Window, when split out
#   OUT  bin/logos_host, bin/ui-host      SEPARATE PROCESSES; they correctly keep
#                                         their own statics
#   OUT  plugins/*/*_plugin.*             a ui_qml backend is loaded by ui-host,
#                                         not by the app
#   OUT  modules/**                       loaded by logos_host
#
# negativeControl = true builds the same script against a tree with a REAL
# duplicate planted where a consumer goes, and asserts the gate REJECTS it.
# Without that, an absence assertion is indistinguishable from a broken one.
{ pkgs, appPkg, negativeControl ? false }:

let
  isDarwin = pkgs.stdenv.isDarwin;
  # Mach-O: -gU is defined externals. ELF: -D --defined-only. mingw nm reads PE.
  definedCmd = if isDarwin then "nm -gU" else "nm -D --defined-only";
  # A count over a tool that read nothing is not evidence of absence. The same
  # defence is written at nix/app.nix:382.
  totalCmd   = if isDarwin then "nm -a" else "nm -D";
in
pkgs.runCommand "logos-basecamp-symbol-gate${pkgs.lib.optionalString negativeControl "-negative"}" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.findutils pkgs.gnugrep pkgs.gnused ]
    ++ [ pkgs.stdenv.cc.bintools ];
} ''
  set -uo pipefail

  # TIER 1 — the split-brain itself. No allowance, ever.
  TIER1_RE='^(TokenManager|StoreRegistry)::|^(vtable|typeinfo|typeinfo name|guard variable) for (TokenManager|StoreRegistry)\b'
  # TIER 2 — LogosAPI/LogosAPIClient. Zero since the single-provider shim was
  # extended to every platform; it was 26 on aarch64-darwin before that
  # (LogosAPI duplicated in the exe while TokenManager was not — benign only
  # because LogosAPI's ctor caches &TokenManager::instance(), which bound to the
  # provider). Never raise this.
  TIER2_RE='^(LogosAPI|LogosAPIClient)::|^(vtable|typeinfo|typeinfo name|guard variable) for (LogosAPI|LogosAPIClient)\b'
  TIER2_ALLOW=0

  FAIL=0
  note() { printf '  %-56s %s\n' "$1" "$2"; }
  bad()  { FAIL=1; printf '  %-56s %s\n' "$1" "$2"; }

  # nix wraps binaries TWO ways and both defeat a naive measurement:
  #   * a shell wrapper that execs bin/.<name>   (nix/app.nix's QT_PLUGIN_PATH
  #     wrapper) -- nm reads ZERO symbols from it
  #   * makeBinaryWrapper's COMPILED stub beside bin/.<name>-wrapped -- nm reads
  #     ~10 symbols from it, so the "did nm read anything" guard below does NOT
  #     catch this one. Measured in logos-logoscore-cli: bin/logoscore is such a
  #     stub, and measuring it reports 0 runtime symbols for a binary that in
  #     fact imports 12.
  # So the rule is deterministic rather than heuristic: if a hidden sibling
  # exists, it IS the image, and we never measure bin/<name>.
  resolve_image() {
    local f="$1" d b real
    d=$(dirname "$f"); b=$(basename "$f")
    for cand in "$d/.$b-wrapped" "$d/.$b"; do
      [ -e "$cand" ] && { printf '%s\n' "$cand"; return; }
    done
    if head -c2 "$f" 2>/dev/null | grep -q '#!'; then
      real=$(sed -nE 's/^exec "\$BINDIR\/([^"]+)".*/\1/p' "$f" | tail -1)
      [ -n "$real" ] && [ -e "$d/$real" ] && { printf '%s\n' "$d/$real"; return; }
    fi
    printf '%s\n' "$f"
  }
  names() { ${definedCmd} "$1" 2>/dev/null | c++filt 2>/dev/null | sed -E 's/^[0-9a-fA-F]+ [A-Za-z] //'; }
  valid() {
    local t; t=$(${totalCmd} "$1" 2>/dev/null | wc -l | tr -d ' ')
    [ "''${t:-0}" -gt 0 ] || { bad "$(basename "$1")" "ERROR: nm read 0 symbols — vacuous"; return 1; }
  }

  ROOT=$TMPDIR/bundle
  mkdir -p "$ROOT"
  cp -R ${appPkg}/. "$ROOT"/ 2>/dev/null || true
  chmod -R u+w "$ROOT"

  PROVIDER=""
  for c in "$ROOT/lib/liblogos_core.dylib" "$ROOT/lib/liblogos_core.so" "$ROOT/bin/liblogos_core.dll"; do
    [ -e "$c" ] && PROVIDER="$c" && break
  done
  [ -n "$PROVIDER" ] || { echo "FATAL: no liblogos_core under the bundle"; exit 1; }

  ${pkgs.lib.optionalString negativeControl ''
    # Plant a REAL second copy of the runtime where an in-process consumer goes.
    mkdir -p "$ROOT/plugins/negative_control"
    cp "$PROVIDER" "$ROOT/plugins/negative_control/main_ui''${PROVIDER##*liblogos_core}"
    echo "NEGATIVE CONTROL: planted a duplicate runtime; the gate MUST reject this tree."
  ''}

  CONSUMERS=()
  for e in "$ROOT/bin/LogosBasecamp" "$ROOT/bin/LogosBasecamp.exe"; do
    [ -e "$e" ] && CONSUMERS+=("$(resolve_image "$e")")
  done
  # -L is load-bearing: a nix output can stage plugins as SYMLINKS into the
  # store, and `find -type f` does NOT match a symlink -- it would return
  # nothing and the gate would assert over an empty consumer set. Measured in
  # logos-logoscore-cli, where `find` saw 0 of 2 real libraries. `[ -f ]`
  # elsewhere is fine, because test(1) follows symlinks and find does not.
  while IFS= read -r p; do [ -n "$p" ] && CONSUMERS+=("$(resolve_image "$p")"); done < <(
    find -L "$ROOT/plugins" -type f \( -name '*_replica_factory.*' -o -name 'main_ui.*' \) 2>/dev/null \
      | grep -Ev '\.(txt|json)$' || true)

  echo "provider  = ''${PROVIDER#$ROOT/}"
  printf 'consumers = '; for c in "''${CONSUMERS[@]}"; do printf '%s ' "''${c#$ROOT/}"; done; echo

  # Positive control AND demangler validity check in one: if c++filt were absent
  # or broken this reports 0 and the gate fails, rather than passing vacuously.
  echo
  echo "== the provider DEFINES the runtime (expect >=1) =="
  valid "$PROVIDER" || exit 1
  n=$(names "$PROVIDER" | grep -Ec 'TokenManager::instance' || true)
  if [ "$n" -ge 1 ]; then note "liblogos_core  TokenManager::instance" "$n  OK"
  else bad "liblogos_core  TokenManager::instance" "$n  EXPECTED >=1 (or demangling is broken)"; fi

  echo
  echo "== TIER 1: no in-process consumer defines TokenManager/StoreRegistry (expect 0) =="
  for img in "''${CONSUMERS[@]}"; do
    valid "$img" || continue
    n=$(names "$img" | grep -Ec "$TIER1_RE" || true)
    if [ "$n" -eq 0 ]; then note "$(basename "$img")" "0  OK"
    else bad "$(basename "$img")" "$n  SPLIT-BRAIN"; names "$img" | grep -E "$TIER1_RE" | sed 's/^/      /' | head -6; fi
  done

  echo
  echo "== TIER 2: LogosAPI/LogosAPIClient duplication (expect <=$TIER2_ALLOW) =="
  T2=0
  for img in "''${CONSUMERS[@]}"; do
    n=$(names "$img" | grep -Ec "$TIER2_RE" || true); T2=$((T2 + n)); note "$(basename "$img")" "$n"
  done
  if [ "$T2" -le "$TIER2_ALLOW" ]; then note "tier-2 total" "$T2  OK"
  else bad "tier-2 total" "$T2  > $TIER2_ALLOW  REGRESSION"; fi

  echo
  echo "== non-weak statics defined in MORE THAN ONE in-process image (expect 0) =="
  ${if isDarwin then ''
    G=$TMPDIR/guards; rm -rf "$G"; mkdir -p "$G"
    for img in "$PROVIDER" "''${CONSUMERS[@]}"; do
      [ -e "$img" ] || continue
      nm -m "$img" 2>/dev/null | c++filt 2>/dev/null | grep 'guard variable for' \
        | grep -v 'weak external' | sed -E 's/.*guard variable for /guard variable for /' \
        | sort -u > "$G/$(basename "$img").g" || true
    done
    DUPES=$(cat "$G"/*.g 2>/dev/null | sort | uniq -d || true)
    if [ -z "$DUPES" ]; then note "cross-image non-weak guard vars" "0  OK"
    else bad "cross-image non-weak guard vars" "$(printf '%s\n' "$DUPES" | wc -l | tr -d ' ')"; printf '%s\n' "$DUPES" | sed 's/^/      /' | head -8; fi
  '' else ''
    note "cross-image guard vars" "skipped (ELF interposes; nm -m is Mach-O only)"
  ''}

  echo
  ${if negativeControl then ''
    if [ "$FAIL" -ne 0 ]; then
      echo "NEGATIVE CONTROL: PASS — the gate correctly rejected a planted duplicate."
      mkdir -p $out; echo ok > $out/result; exit 0
    else
      echo "NEGATIVE CONTROL: FAIL — the gate ACCEPTED a planted duplicate. It is vacuous."
      exit 1
    fi
  '' else ''
    if [ "$FAIL" -eq 0 ]; then
      echo "SYMBOL GATE: PASS"; mkdir -p $out; echo ok > $out/result; exit 0
    else
      echo "SYMBOL GATE: FAIL — see logos-protocol/cpp/logos_shared_api.h"; exit 1
    fi
  ''}
''
