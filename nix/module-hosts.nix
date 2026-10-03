# Every module host liblogos ships must be in the bundle. liblogos starts each
# module in one, looked for next to the program; a missing host fails every
# module that needs it ("logos_host_plain not found").
{ pkgs, appPkg, logosLiblogos }:

pkgs.runCommand "logos-basecamp-module-hosts" { } ''
  missing=0
  for host in "${logosLiblogos}"/bin/logos_host*; do
    [ -e "$host" ] || continue
    name=$(basename "$host")
    # logos_host_qt is bundled under its compatibility name, logos_host.
    case "$name" in logos_host_qt|logos_host_qt.exe) continue ;; esac
    if [ ! -e "${appPkg}/bin/$name" ]; then
      echo "the bundle is missing $name, which liblogos ships" >&2
      missing=1
    fi
  done
  [ "$missing" -eq 0 ]
  touch $out
''
