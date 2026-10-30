#!/usr/bin/env bash
# Stage the intent fixtures into a basecamp --user-dir.
#
# Copies the already-installed apps the intents-fixtures flake output carries
# (built by build-installed.sh, content hashes included), so this needs nothing
# but bash -- it runs the same on Windows. From a source checkout, run
# build-installed.sh instead.
#
# Usage: stage.sh <user-dir>
set -euo pipefail

USER_DIR="${1:?usage: stage.sh <user-dir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALLED="$HERE/installed/plugins"

if [ ! -d "$INSTALLED" ]; then
    echo "stage.sh: $INSTALLED not found — use the intents-fixtures flake output," >&2
    echo "or run build-installed.sh from a source checkout" >&2
    exit 1
fi

mkdir -p "$USER_DIR/plugins"
for app_dir in "$INSTALLED"/*/; do
    app="$(basename "$app_dir")"
    rm -rf "$USER_DIR/plugins/$app"
    cp -R "$app_dir" "$USER_DIR/plugins/$app"
    chmod -R u+w "$USER_DIR/plugins/$app"
    echo "staged $app -> $USER_DIR/plugins/$app"
done
