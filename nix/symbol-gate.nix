# The one-runtime symbol gate.
#
# INVARIANT: across the images that share ONE process, each runtime type is
# DEFINED by exactly ONE image. A second definition is a second TokenManager:
# the host writes a capability token into one store, another image reads an
# empty one, and every cross-module call is refused at runtime with NO build
# diagnostic.
#
# The invariant is EXACTLY-ONE, deliberately, rather than "liblogos_core is the
# provider". It used to be the latter, because liblogos_core absorbed both
# static archives and re-exported them. It is not the provider any more:
# liblogos_protocol owns TokenManager/LogosAPIClient and liblogos_qt_host owns
# LogosAPI, and liblogos_core imports both like everyone else. Naming an owner
# here would have to be edited every time ownership moves -- and the first time
# it moved, this check failed while every real assertion still passed. Ownership
# is declared in logos-protocol/cpp/logos_shared_api.h. (Earlier revisions of
# this comment also pointed at cmake/LogosSharedFromDll.cmake; that file was the
# single-provider shim and was deleted in #348 once the runtime became real
# shared libraries, so there is nothing to follow there any more.)
#
# Nothing else in the tree measures this, and it has no build diagnostic: it
# surfaces at runtime as "ModuleProxy: rejecting unauthorized call" and
# "LogosAPIClient: No token found for module".
#
# THE IN-PROCESS IMAGE SET — this scoping IS the correctness of the gate:
#   IN   bin/LogosBasecamp                the app
#   IN   lib/liblogos_core.*              the single provider
#   IN   plugins/*/*_replica_factory.*    QPluginLoader'd by LogosQmlBridge
#   IN   plugins/main_ui/main_ui.*        QPluginLoader'd by Window
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
  isDarwin  = pkgs.stdenv.isDarwin;
  isWindows = pkgs.stdenv.hostPlatform.isWindows;
  # Mach-O: -gU is defined externals. ELF: -D --defined-only.
  #
  # PE gets NEITHER, because a PE has no ELF dynamic symbol table and `nm -D`
  # therefore reads nothing at all from a .dll. Measured against a real mingw
  # PE (libffi-8.dll, binutils 2.46): `nm -D` -> 0 lines and an error on stderr,
  # `nm` -> 687, `nm --defined-only` -> 686. Left as -D, valid() below would see
  # zero symbols and abort the whole gate as vacuous before it asserted
  # anything -- loud rather than silent, but the gate would never once have run
  # on the platform it matters most on. PE has no symbol interposition, so a
  # duplicate runtime that Linux and macOS quietly collapse to one is fatal
  # there and only there.
  definedCmd = if isDarwin then "nm -gU"
               else if isWindows then "nm --defined-only"
               else "nm -D --defined-only";
  # A count over a tool that read nothing is not evidence of absence. nix/app.nix
  # writes the same defence over its Qt DLL closure.
  totalCmd   = if isDarwin then "nm -a"
               else if isWindows then "nm"
               else "nm -D";
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
  #     catch this one (logos-logoscore-cli's bin/logoscore is such a stub: it
  #     measures 0 runtime symbols for a binary that in fact imports 12).
  # Hence the deterministic rule: a hidden sibling IS the image, and we never
  # measure bin/<name>.
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
    #
    # It plants liblogos_protocol, NOT liblogos_core. liblogos_core used to
    # absorb both static archives and was therefore a genuine second copy of the
    # runtime; since it started IMPORTING instead, it defines zero runtime
    # symbols, so planting it plants NOTHING -- the gate accepted the tree and
    # this control failed, correctly reporting itself vacuous. A negative
    # control has to duplicate a DEFINER, so it has to track where the
    # definitions actually live.
    mkdir -p "$ROOT/plugins/negative_control"
    _definer=""
    for c in "$ROOT/lib/liblogos_protocol.dylib" "$ROOT/lib/liblogos_protocol.so" "$ROOT/bin/liblogos_protocol.dll"; do
      [ -e "$c" ] && _definer="$c" && break
    done
    [ -n "$_definer" ] || { echo "NEGATIVE CONTROL: no liblogos_protocol to plant"; exit 1; }
    cp "$_definer" "$ROOT/plugins/negative_control/main_ui.''${_definer##*.}"
    echo "NEGATIVE CONTROL: planted a duplicate definer; the gate MUST reject this tree."
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

  # Every image that shares the process, providers included: ownership is what
  # is being asserted, so nothing may be exempt from the count.
  ALL=("$PROVIDER" "''${CONSUMERS[@]}")
  while IFS= read -r p; do
    [ -n "$p" ] && [ "$p" != "$PROVIDER" ] && ALL+=("$p")
  # bin/ is searched too, and .dll is matched: on Windows every liblogos_* shared
  # image is staged into bin/, NOT lib/ (nix/app.nix flips _libdest for exactly
  # that reason -- the PE loader searches the executable's directory). Globbing
  # lib/*.{dylib,so} alone found ZERO definers on Windows, so the exactly-one
  # assertion below reported "0 definers" for every family and could not pass on
  # a correct tree. The three sibling probes above -- provider, negative control,
  # consumer -- each learned this layout; this one had not.
  done < <(find -L "$ROOT/lib" "$ROOT/bin" -maxdepth 1 -type f \
    \( -name 'liblogos_*.dylib' -o -name 'liblogos_*.so' -o -name 'liblogos_*.dll' \) 2>/dev/null \
    | grep -v 'liblogos_core' || true)

  # Exactly one definer per type. This doubles as the demangler validity
  # control: if c++filt were absent or broken every family would report ZERO
  # definers and fail, rather than every consumer reporting a reassuring zero.
  echo
  echo "== each runtime type is defined by EXACTLY ONE image =="
  valid "$PROVIDER" || exit 1
  # StoreRegistry is deliberately absent from this list: token_manager.cpp
  # defines `static StoreRegistry r;` inside registry(), so it is file-local
  # (1 symbol under `nm -a`, 0 under `nm -gU`) and requiring exactly one DEFINER
  # of something never exported would fail forever. It stays in the TIER 1 scan
  # below, where "no consumer defines it" is meaningful precisely because it
  # should never become external.
  for fam in TokenManager LogosAPI LogosAPIClient; do
    _n=0; _owners=""
    for img in "''${ALL[@]}"; do
      [ -e "$img" ] || continue
      c=$(names "$img" | grep -cE "^''${fam}::|^(vtable|typeinfo|typeinfo name|guard variable) for ''${fam}\b" || true)
      if [ "$c" -gt 0 ]; then _n=$((_n + 1)); _owners="$_owners $(basename "$img")($c)"; fi
    done
    if [ "$_n" -eq 1 ]; then note "$fam" "1 definer:$_owners  OK"
    else bad "$fam" "$_n definers:$_owners  EXPECTED exactly 1"; fi
  done

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
