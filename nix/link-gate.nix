# The duplicate-library link gate.
#
# INVARIANT: where the bundle stages one library name twice -- in lib/ and
# beside a module -- both copies must satisfy that module's imports. macOS
# binds to lib/ (bundle.sh rewrites the @rpath dep flat), Linux to the sibling
# ($ORIGIN first), so a skew kills exactly one platform; and Mach-O binds
# lazily, so it dies at the first call, not at load. That was #361's
# "Module process crashed: package_manager", with every Linux job green.
#
# Windows is out: a PE has no `nm -D` symbol table. negativeControl plants a
# real skew and asserts the gate REJECTS it.
{ pkgs, bundlePkg, negativeControl ? false }:

let
  isDarwin = pkgs.stdenv.isDarwin;
  tp  = pkgs.stdenv.cc.targetPrefix;
  ext = if isDarwin then "dylib" else "so";
  fmt = if isDarwin then "macho" else "elf";
in
pkgs.runCommand "logos-basecamp-link-gate${pkgs.lib.optionalString negativeControl "-negative"}" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.python3 pkgs.stdenv.cc.bintools ];
} ''
  set -uo pipefail

  ROOT=$TMPDIR/bundle
  mkdir -p "$ROOT"
  cp -R ${bundlePkg}/. "$ROOT"/ 2>/dev/null || true
  chmod -R u+w "$ROOT"

  ${pkgs.lib.optionalString negativeControl ''
    # A different library under a duplicated name: lib/ then satisfies none of
    # the imports the sibling copy satisfies, which is the shape of the bug.
    for f in "$ROOT/lib/liblgx.${ext}" "$ROOT/lib/liblogos_core.${ext}"; do
      [ -e "$f" ] || { echo "NEGATIVE CONTROL: $f absent, nothing to plant"; exit 1; }
    done
    cp "$ROOT/lib/liblogos_core.${ext}" "$ROOT/lib/liblgx.${ext}"
    echo "NEGATIVE CONTROL: planted a skewed lib/liblgx.${ext}; the gate MUST reject this tree."
    echo
  ''}

  # `|| rc=$?` because runCommand runs this under `set -e`: a bare non-zero
  # exit here aborts before the gate can report its own verdict.
  rc=0
  python3 ${./link-gate.py} "$ROOT" "${tp}nm" "${fmt}" || rc=$?

  echo
  ${if negativeControl then ''
    if [ "$rc" -ne 0 ]; then
      echo "NEGATIVE CONTROL: PASS — the gate rejected a planted skew."
      mkdir -p $out; echo ok > $out/result; exit 0
    fi
    echo "NEGATIVE CONTROL: FAIL — the gate ACCEPTED a planted skew. It is vacuous."
    exit 1
  '' else ''
    if [ "$rc" -eq 0 ]; then
      echo "LINK GATE: PASS"; mkdir -p $out; echo ok > $out/result; exit 0
    fi
    echo "LINK GATE: FAIL — one bundled library name, two different builds."
    exit 1
  ''}
''
