#!/usr/bin/env bash
# Refresh src/Basecamp/Installer from basecamp-installer-common: the revision
# flake.lock pins, or a checkout given with --from (while changing both).
#
#   scripts/sync-installer-common.sh [--from <basecamp-installer-common checkout>]
set -euo pipefail
cd "$(dirname "$0")/.."

source_dir=""
if [ "${1:-}" = "--from" ]; then
    source_dir="${2:?--from needs a path}"
else
    pin=$(jq -r '.nodes as $n | $n[$n.root.inputs["basecamp-installer-common"]].locked
                 | "\(.type):\(.owner)/\(.repo)/\(.rev)"' flake.lock)
    case "$pin" in
        *null*) echo "flake.lock pins no basecamp-installer-common: run nix flake lock first" >&2; exit 1 ;;
    esac
    source_dir=$(nix flake prefetch "$pin" --json | jq -r .storePath)
fi

[ -d "$source_dir/qml/Installer" ] || { echo "$source_dir has no qml/Installer" >&2; exit 1; }
rm -f src/Basecamp/Installer/*.js
mkdir -p src/Basecamp/Installer
cp "$source_dir"/qml/Installer/*.js src/Basecamp/Installer/
chmod u+w src/Basecamp/Installer/*.js
echo "src/Basecamp/Installer: from $source_dir"
echo "The files are listed in src/CMakeLists.txt and tests/qml/CMakeLists.txt."
