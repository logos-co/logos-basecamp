#!/usr/bin/env bash
set -euo pipefail

usage() { echo "Usage: $0 --bundle <path> --output <path.zip>"; exit 1; }

BUNDLE=""
OUTPUT=""
while [[ $# -gt 0 ]]; do
  case $1 in
    --bundle) BUNDLE="$2"; shift 2 ;;
    --output) OUTPUT="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[[ -z "$BUNDLE" || -z "$OUTPUT" ]] && usage

src=$(readlink -f "$BUNDLE")

# Verify bundle is not empty
staged=$(find -L "$src" -type f | wc -l | tr -d ' ')
[[ "$staged" -gt 0 ]] || { echo "ERROR: bundle is empty"; exit 1; }

# Clean previous staging directory
rm -rf pkg/logos-basecamp

# Copy with dereferenced symlinks
mkdir -p pkg/logos-basecamp
cp -rL "$src"/. pkg/logos-basecamp/

# Resolve output path (handles both relative and absolute paths)
OUTPUT_DIR=$(dirname "$OUTPUT")
OUTPUT_NAME=$(basename "$OUTPUT")
mkdir -p "$OUTPUT_DIR"
OUTPUT_ABS=$(cd "$OUTPUT_DIR" && pwd)/"$OUTPUT_NAME"

# Remove old zip if exists
rm -f "$OUTPUT_ABS"

# Create zip (use env var to avoid quoting issues with apostrophes)
export OUTPUT_ABS
nix-shell -p zip --run 'cd pkg && zip -qr "$OUTPUT_ABS" logos-basecamp'

# Verify zip contents
export OUTPUT_ABS
zipped=$(nix-shell -p unzip --run 'unzip -l "$OUTPUT_ABS"' | tail -1 | awk '{print $2}')
echo "bundle $staged file(s) -> zip $zipped entr(ies)"

if [[ "$zipped" -lt "$staged" ]]; then
  echo "ERROR: zip has $zipped entries but bundle has $staged files."
  echo "ERROR: Symlinks not dereferenced; DLLs would be missing."
  exit 1
fi

n_exe=$(find pkg/logos-basecamp -name '*.exe' | wc -l | tr -d ' ')
[[ "$n_exe" -gt 0 ]] || { echo "ERROR: no .exe in bundle"; exit 1; }
echo "exe(s): $n_exe"
echo "Created: $OUTPUT"