# Every host liblogos ships must be in the bundle: logos_runtime, which the app
# spawns, and the module hosts the runtime starts each module in. Both are looked
# for next to the program; a missing one fails the app or every module that
# needs it ("logos_host_plain not found").
{ pkgs, appPkg, logosLiblogos }:

pkgs.runCommand "logos-basecamp-module-hosts" { } ''
  missing=0
  found=0
  for host in "${logosLiblogos}"/bin/logos_host* "${logosLiblogos}"/bin/logos_runtime*; do
    [ -e "$host" ] || continue
    name=$(basename "$host")
    # logos_host_qt is bundled under its compatibility name, logos_host.
    case "$name" in logos_host_qt|logos_host_qt.exe) continue ;; esac
    found=$((found + 1))
    if [ ! -e "${appPkg}/bin/$name" ]; then
      echo "the bundle is missing $name, which liblogos ships" >&2
      missing=1
    fi
  done
  if [ "$found" -eq 0 ]; then
    echo "liblogos ships no hosts at ${logosLiblogos}/bin, so nothing was checked" >&2
    missing=1
  fi
  [ "$missing" -eq 0 ]
  touch $out
''
