#!/usr/bin/env bash
set -uxo pipefail

###############################################################################
# macOS Code Signing & Notarization for LogosApp.app
#
# Supports modes:
#   --mode sign      - Sign only
#   --mode notarize  - Notarize only (requires pre-signed app)
#   --mode both      - Sign and notarize (default)
#
# Usage:
#   ./scripts/sign-and-notarize.sh [--dmg PATH] [--bundle PATH] [--output PATH] [--mode MODE] [--timeout TIMEOUT]
#
# Required env vars:
#   MACOS_CODESIGN_IDENT       - Developer ID Application identity
#   MACOS_NOTARY_ISSUER_ID     - App Store Connect API issuer ID
#   MACOS_NOTARY_KEY_ID        - App Store Connect API key ID
#   MACOS_NOTARY_KEY_FILE      - Path to .p8 AuthKey file
#   MACOS_KEYCHAIN_PASS        - Password for the .p12 certificate
#   MACOS_KEYCHAIN_FILE        - Path to the .p12 certificate file
###############################################################################

# Parse arguments
DMG_PATH=""
BUNDLE_PATH=""
OUTPUT_PATH=""
MODE="both"
TIMEOUT="30m"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dmg)
      DMG_PATH="$2"
      shift 2
      ;;
    --bundle)
      BUNDLE_PATH="$2"
      shift 2
      ;;
    --output)
      OUTPUT_PATH="$2"
      shift 2
      ;;
    --mode)
      MODE="$2"
      shift 2
      ;;
    --timeout)
      TIMEOUT="$2"
      shift 2
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

# Validate mode
if [[ ! "$MODE" =~ ^(sign|notarize|both)$ ]]; then
  echo "Error: --mode must be 'sign', 'notarize', or 'both'" >&2
  exit 1
fi

# Create temp directory for all operations (needed for Nix read-only store paths)
TEMP_DIR=$(mktemp -d)
trap "rm -rf '$TEMP_DIR'" EXIT

# Determine APP_BUNDLE path and copy to writable temp directory
if [[ -n "$DMG_PATH" ]]; then
  [[ -f "$DMG_PATH" ]] || { echo "Error: DMG not found: $DMG_PATH" >&2; exit 1; }
  
  echo "Extracting DMG."
  MOUNT_POINT="${TEMP_DIR}/mnt"
  APP_BUNDLE="${TEMP_DIR}/LogosApp.app"
  
  mkdir -p "$MOUNT_POINT"
  hdiutil attach -readonly -mountpoint "$MOUNT_POINT" "$DMG_PATH"
  cp -a "$MOUNT_POINT/LogosApp.app" "$APP_BUNDLE"
  hdiutil detach "$MOUNT_POINT"
elif [[ -n "$BUNDLE_PATH" ]]; then
  [[ -d "$BUNDLE_PATH" ]] || { echo "Error: Bundle not found: $BUNDLE_PATH" >&2; exit 1; }
  
  echo "Copying bundle to temp directory."
  APP_BUNDLE="${TEMP_DIR}/LogosApp.app"
  cp -a "$BUNDLE_PATH" "$APP_BUNDLE"
  chmod -R u+w "$APP_BUNDLE"
else
  # Default: current directory
  [[ -d "./LogosApp.app" ]] || { echo "Error: Bundle not found at ./LogosApp.app" >&2; exit 1; }
  
  echo "Copying bundle to temp directory."
  APP_BUNDLE="${TEMP_DIR}/LogosApp.app"
  cp -a "./LogosApp.app" "$APP_BUNDLE"
  chmod -R u+w "$APP_BUNDLE"
fi

CONTENTS="${APP_BUNDLE}/Contents"
ENTITLEMENTS="${TEMP_DIR}/entitlements.plist"
KEYCHAIN_NAME="build.keychain"
KEYCHAIN_PATH="${HOME}/Library/Keychains/${KEYCHAIN_NAME}"
KEYCHAIN_DB_PATH="${KEYCHAIN_PATH}-db"
CERT_DIR="/Users/jenkins/certs"

# Codesign options — hardened runtime required for notarization
# Try to extract identity from env var or use the full identity string
CODESIGN_OPTS=(
    --force
    --timestamp
    --options runtime
    --sign "${MACOS_CODESIGN_IDENT}"
)

###############################################################################
# SIGN MODE
###############################################################################
if [[ "$MODE" =~ ^(sign|both)$ ]]; then
  echo "Starting signing phase."
  
  ###############################################################################
  # 0. Create entitlements file
  ###############################################################################
  cat > "${ENTITLEMENTS}" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.allow-jit</key>
    <true/>
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <true/>
    <key>com.apple.security.cs.allow-dyld-environment-variables</key>
    <true/>
    <key>com.apple.security.cs.disable-library-validation</key>
    <true/>
</dict>
</plist>
EOF

  ###############################################################################
  # 0.5 Remove all existing signatures (critical!)
  ###############################################################################
  echo "Removing existing signatures..."
  find "${APP_BUNDLE}" -type f -name "_CodeSignature" -exec rm -rf {} + 2>/dev/null || true
  find "${APP_BUNDLE}" -type d -name "_CodeSignature" -exec rm -rf {} + 2>/dev/null || true

  ###############################################################################
  # 1. Set up a temporary keychain and import the certificate
  ###############################################################################
  echo "Setting up keychain at ${KEYCHAIN_DB_PATH}"
  security delete-keychain "${KEYCHAIN_DB_PATH}" 2>/dev/null || true
  security create-keychain -p "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_PATH}"
  security unlock-keychain -p "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_PATH}"
  security set-keychain-settings -lut 21600 "${KEYCHAIN_PATH}"

  ###############################################################################
  # 2. Import Trust Chain (No more curl!)
  ###############################################################################
  echo "Importing Apple Trust Chain from local storage..."
  
  # Import the specific G2 files you just copied over
  security import "${CERT_DIR}/DeveloperIDG2CA.cer" -k "${KEYCHAIN_DB_PATH}" -t cert
  security import "${CERT_DIR}/AppleRootCA-G2.cer" -k "${KEYCHAIN_DB_PATH}" -t cert
  security import "${CERT_DIR}/AppleWWDRCAG2.cer" -k "${KEYCHAIN_DB_PATH}" -t cert

  # Ensure the system looks at our build keychain first
  security list-keychains -d user -s "${KEYCHAIN_DB_PATH}" /Library/Keychains/System.keychain

  ###############################################################################
  # 3. Import Identity and Set Permissions
  ###############################################################################
  echo "Extracting identity from P12..."
  openssl pkcs12 -in "${MACOS_KEYCHAIN_FILE}" \
      -passin pass:"${MACOS_KEYCHAIN_PASS}" \
      -nodes -out /tmp/cert_and_key.pem
  
  echo "Importing identity from PEM..."
  security import /tmp/cert_and_key.pem \
      -k "${KEYCHAIN_DB_PATH}" \
      -T /usr/bin/codesign
  
  # Cleanup PEM
  rm -f /tmp/cert_and_key.pem

  # This is the 'magic' command for Sequoia to allow codesign access
  security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_DB_PATH}"

  echo "Debug: Keychain contents"
  echo "Available keys in ${KEYCHAIN_NAME}:"
  security find-identity -v "${KEYCHAIN_NAME}" || echo "No identities in build.keychain"
  
  echo "Debug: All codesigning identities in all keychains"
  security find-identity -v -p codesigning || echo "No codesigning identities found"
  
  echo "Debug: Dump of build.keychain"
  security dump-keychain "${KEYCHAIN_NAME}" 2>&1 | head -50 || true

  ###############################################################################
  # 2. Sign + repack .lgx archives in Contents/preinstall/
  ###############################################################################
  echo "Signing dylibs inside .lgx archives."
  find "${CONTENTS}/preinstall" -name "*.lgx" 2>/dev/null | while read -r lgx; do
      echo "  Processing: ${lgx}"
      tmpdir=$(mktemp -d)
      gtar -xzf "$lgx" -C "$tmpdir"
      find "$tmpdir" -name "*.dylib" -exec \
          codesign --force --options runtime --sign "${MACOS_CODESIGN_IDENT}" --timestamp {} \;
      gtar -czf "$lgx" -C "$tmpdir" --transform 's|^\./||' .
      rm -rf "$tmpdir"
      echo "  Repacked: ${lgx}"
  done

  ###############################################################################
  # 3. Sign all dylibs in Frameworks/
  ###############################################################################
  echo "Signing dylibs in Frameworks."
  while IFS= read -r dylib; do
      [[ -n "$dylib" ]] || continue
      echo "  Signing: ${dylib}"
      codesign "${CODESIGN_OPTS[@]}" "$dylib"
  done < <(find "${CONTENTS}/Frameworks" -name '*.dylib' -type f)

  ###############################################################################
  # 4. Sign all dylibs in Resources/qt/
  ###############################################################################
  echo "Signing dylibs in Resources/qt."
  while IFS= read -r dylib; do
      [[ -n "$dylib" ]] || continue
      echo "  Signing: ${dylib}"
      codesign "${CODESIGN_OPTS[@]}" "$dylib"
  done < <(find "${CONTENTS}/Resources/qt" -type f -name '*.dylib' 2>/dev/null)

  ###############################################################################
  # 5. Sign Qt frameworks (binary inside Versions/A/ then the .framework dir)
  ###############################################################################
  echo "Signing Qt frameworks."
  find "${CONTENTS}/Frameworks" -name '*.framework' -type d -maxdepth 1 2>/dev/null | sort | while read -r fw; do
      fw_name=$(basename "${fw}" .framework)
      fw_binary="${fw}/Versions/A/${fw_name}"

      if [[ -f "${fw_binary}" ]]; then
          echo "  Signing framework binary: ${fw_binary}"
          codesign "${CODESIGN_OPTS[@]}" "${fw_binary}"
      fi
  done

  ###############################################################################
  # 6. Sign executables in Contents/MacOS/
  ###############################################################################
  echo "Signing executables."
  # Sign all Mach-O binaries first (with entitlements)
  for exe in "${CONTENTS}/MacOS/LogosApp.bin" \
             "${CONTENTS}/MacOS/logos-app" \
             "${CONTENTS}/MacOS/logos_host" \
             "${CONTENTS}/MacOS/logoscore"; do
      if [[ -f "${exe}" ]]; then
          echo "  Signing: ${exe}"
          codesign "${CODESIGN_OPTS[@]}" --entitlements "${ENTITLEMENTS}" "${exe}"
      fi
  done

  # Sign the shell script wrapper last (no --options runtime for scripts)
  if [[ -f "${CONTENTS}/MacOS/LogosApp" ]]; then
    echo "  Signing wrapper: ${CONTENTS}/MacOS/LogosApp"
    codesign --force --timestamp --sign "${MACOS_CODESIGN_IDENT}" "${CONTENTS}/MacOS/LogosApp"
  fi

  ###############################################################################
  # 7. Sign the top-level app bundle
  ###############################################################################
  echo "Signing app bundle."
  codesign "${CODESIGN_OPTS[@]}" --entitlements "${ENTITLEMENTS}" "${APP_BUNDLE}"

  ###############################################################################
  # 8. Verify
  ###############################################################################
  echo "Verifying signature."
  codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}"

  echo "Checking Gatekeeper assessment."
  spctl --assess --type execute --verbose=2 "${APP_BUNDLE}" || true

  echo "Signing phase complete"
fi

###############################################################################
# NOTARIZE MODE
###############################################################################
if [[ "$MODE" =~ ^(notarize|both)$ ]]; then
  echo "Starting notarization phase."
  
  ###############################################################################
  # 12. Create ZIP for notarization
  ###############################################################################
  echo "Creating ZIP for notarization."
  NOTARIZE_ZIP="${TEMP_DIR:-/tmp}/LogosApp-$$.zip"
  ditto -c -k --keepParent "${APP_BUNDLE}" "${NOTARIZE_ZIP}"

  ###############################################################################
  # 13. Submit for notarization
  ###############################################################################
  echo "Submitting for notarization."
  xcrun notarytool submit "${NOTARIZE_ZIP}" \
      --issuer "${MACOS_NOTARY_ISSUER_ID}" \
      --key-id "${MACOS_NOTARY_KEY_ID}" \
      --key "${MACOS_NOTARY_KEY_FILE}" \
      --wait \
      --timeout "${TIMEOUT}"

  ###############################################################################
  # 14. Staple the notarization ticket
  ###############################################################################
  echo "Stapling notarization ticket."
  xcrun stapler staple "${APP_BUNDLE}"

  ###############################################################################
  # 15. Final verification
  ###############################################################################
  echo "Final verification."
  spctl --assess --type execute --verbose=2 "${APP_BUNDLE}"
  codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}"

  # Cleanup ZIP and keychain
  rm -f "${NOTARIZE_ZIP}"
  rm -f "${ENTITLEMENTS}"
  security delete-keychain "${KEYCHAIN_NAME}"

  echo "Notarization phase complete"
fi

###############################################################################
# OUTPUT
###############################################################################
if [[ -n "$OUTPUT_PATH" ]]; then
  echo "Creating output DMG."
  mkdir -p "$(dirname "$OUTPUT_PATH")"
  hdiutil create -volname "LogosApp" \
                 -srcfolder "${APP_BUNDLE}" \
                 -ov -format UDZO \
                 -puppetstrings \
                 "${OUTPUT_PATH}"
  echo "Output DMG created: $OUTPUT_PATH"
else
  echo "Done! Bundle signed/notarized: ${APP_BUNDLE}"
fi