#!/usr/bin/env bash
# Stage the intent fixtures into a basecamp --user-dir.
#
# Hand-written 4-file app directories rather than built modules: no nix build,
# no .lgx, no catalog, no network. The pattern is the one the missing-deps
# doctest already uses.
#
# Usage: stage.sh <user-dir>
set -euo pipefail

USER_DIR="${1:?usage: stage.sh <user-dir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGINS="$USER_DIR/plugins"

mkdir -p "$PLUGINS"
for app in intent_requester_demo intent_provider_a intent_provider_b intent_provider_manual; do
    # Human label the chooser shows — deliberately distinct from the module
    # name so the test can tell which string the dialog rendered.
    case "$app" in
        intent_requester_demo) disp="Intent Requester" ;;
        intent_provider_a)     disp="Provider A" ;;
        intent_provider_b)     disp="Provider B" ;;
        intent_provider_manual) disp="Manual Provider" ;;
        *)                     disp="$app" ;;
    esac
    dest="$PLUGINS/$app"
    mkdir -p "$dest"
    cp "$HERE/$app/metadata.json" "$dest/"
    cp "$HERE/$app/Main.qml"      "$dest/"
    # manifest.json is what the package manager lists installed packages from;
    # metadata.json is what IntentRegistry reads uses/provides out of. Both are
    # needed: an app missing from the manifest never reaches the registry.
    # Shape copied from doctests/basecamp-missing-deps.test.yaml, which is the
    # authority for "what an installed QML-only app looks like on disk".
    # `main` is an OBJECT (variant -> binary) and is empty for a QML-only app;
    # a string there is not what the package scanner reads.
    cat > "$dest/manifest.json" <<JSON
{
  "author": "",
  "category": "testing",
  "dependencies": [],
  "description": "Intent test fixture (test artifact, not a real app)",
  "display_name": "$disp",
  "icon": "",
  "main": {},
  "manifestVersion": "0.3.0",
  "name": "$app",
  "type": "ui_qml",
  "version": "1.0.0",
  "view": "Main.qml"
}
JSON
    echo "staged $app -> $dest"
done
