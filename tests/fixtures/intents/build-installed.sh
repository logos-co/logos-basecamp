#!/usr/bin/env bash
# Install the intent fixtures into a basecamp --user-dir, the way lgpm installs
# a package (content hashes included: Basecamp refuses a user-installed package
# without them).
#
# Hand-written 4-file apps rather than built modules: no nix build, no catalog,
# no network. Needs node and LGX_CLI (the lgx CLI). The intents-fixtures flake
# output runs this at build time, so stage.sh can copy the result anywhere.
#
# Usage: build-installed.sh <user-dir>
set -euo pipefail

USER_DIR="${1:?usage: build-installed.sh <user-dir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

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
    src="$WORK/$app"
    mkdir -p "$src"
    cp "$HERE/$app/metadata.json" "$src/"
    cp "$HERE/$app/Main.qml"      "$src/"
    # manifest.json is what the package manager lists installed packages from;
    # metadata.json is what IntentRegistry reads uses/provides out of. Both are
    # needed: an app missing from the manifest never reaches the registry.
    # `main` is an OBJECT (variant -> binary) and is empty for a QML-only app;
    # a string there is not what the package scanner reads.
    cat > "$src/manifest.json" <<JSON
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
    node "$HERE/../install-fixture.mjs" "$USER_DIR" "$src" >/dev/null
    echo "installed $app -> $USER_DIR/plugins/$app"
done
