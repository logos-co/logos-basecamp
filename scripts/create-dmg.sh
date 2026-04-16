#!/usr/bin/env bash
set -euo pipefail

# Usage: ./scripts/create-dmg.sh --bundle PATH --output PATH

BUNDLE_PATH=""
OUTPUT_PATH=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bundle) BUNDLE_PATH="$2"; shift 2 ;;
    --output) OUTPUT_PATH="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

[[ -n "$BUNDLE_PATH" ]] || { echo "Error: --bundle is required" >&2; exit 1; }
[[ -n "$OUTPUT_PATH" ]] || { echo "Error: --output is required" >&2; exit 1; }
[[ -d "$BUNDLE_PATH" ]] || { echo "Error: Bundle not found: $BUNDLE_PATH" >&2; exit 1; }

mkdir -p "$(dirname "$OUTPUT_PATH")"

DMG_STAGING=$(mktemp -d)
trap "chmod -R u+w '${DMG_STAGING}' 2>/dev/null; rm -rf '${DMG_STAGING}'" EXIT

cp -a "$BUNDLE_PATH" "${DMG_STAGING}/"
ln -s /Applications "${DMG_STAGING}/Applications"

hdiutil create -volname "LogosBasecamp" \
    -srcfolder "${DMG_STAGING}" \
    -ov -format UDZO \
    -puppetstrings \
    "${OUTPUT_PATH}"

echo "DMG created: ${OUTPUT_PATH}"