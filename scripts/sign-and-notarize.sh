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
KEYCHAIN_PATH="${HOME}/Library/Keychains/${KEYCHAIN_NAME}-db"

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
  echo "Setting up keychain."
  security delete-keychain "${KEYCHAIN_PATH}" 2>/dev/null || true

  # Create and unlock
  security create-keychain -p "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_PATH}"
  security unlock-keychain -p "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_PATH}"
  # Prevent the keychain from locking during the build
  security set-keychain-settings -lut 21600 "${KEYCHAIN_PATH}"

  # Ensure Apple root certs are available
  echo "Ensuring Apple Root and Intermediate (WWDR) certificates..."
  # We need BOTH the Root and the WWDR Intermediate for Sequoia to validate the cert
  curl -s https://www.apple.com/appleca/AppleIncRootCertificate.cer -o /tmp/AppleRoot.cer
  curl -s https://developer.apple.com/certificationauthority/AppleWWDRCA.cer -o /tmp/AppleWWDR.cer

  # Import them into your SPECIFIC build keychain
  security import /tmp/AppleRoot.cer -k "${KEYCHAIN_PATH}" -A
  security import /tmp/AppleWWDR.cer -k "${KEYCHAIN_PATH}" -A

  # CRITICAL: Set keychain search list BEFORE importing cert
  security list-keychains -d user -s "${KEYCHAIN_PATH}" \
    "$HOME/Library/Keychains/login.keychain-db" \
    "/Library/Keychains/System.keychain"

  # Import cert
  security import "${MACOS_KEYCHAIN_FILE}" \
      -k "${KEYCHAIN_PATH}" \
      -P "${MACOS_KEYCHAIN_PASS}" \
      -T /usr/bin/codesign \
      -T /usr/bin/security \
      -A

  security set-key-partition-list \
      -S apple-tool:,apple:,codesign: \
      -s -k "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_PATH}"

  echo "Debug: Keychain contents"
  echo "Available keys in ${KEYCHAIN_NAME}:"
  security find-identity -v "${KEYCHAIN_PATH}" || echo "No identities in build.keychain"
  
  echo "Debug: All codesigning identities in all keychains"
  security find-identity -v -p codesigning || echo "No codesigning identities found"
  
  echo "Debug: Dump of build.keychain"
  security dump-keychain "${KEYCHAIN_PATH}" 2>&1 | head -50 || true

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

  # Cleanup keychain after signing
  echo "Cleaning up keychain."
  security delete-keychain "${KEYCHAIN_NAME}"
  rm -f "${ENTITLEMENTS}"

  echo "Signing phase complete"
fi

###############################################################################
# NOTARIZE MODE
###############################################################################
if [[ "$MODE" =~ ^(notarize|both)$ ]]; then
  echo "Starting notarization phase."
  
  ###############################################################################
  # 9. Create ZIP for notarization
  ###############################################################################
  echo "Creating ZIP for notarization."
  NOTARIZE_ZIP="${TEMP_DIR:-/tmp}/LogosApp-$$.zip"
  ditto -c -k --keepParent "${APP_BUNDLE}" "${NOTARIZE_ZIP}"

  ###############################################################################
  # 10. Submit for notarization
  ###############################################################################
  echo "Submitting for notarization."
  xcrun notarytool submit "${NOTARIZE_ZIP}" \
      --issuer "${MACOS_NOTARY_ISSUER_ID}" \
      --key-id "${MACOS_NOTARY_KEY_ID}" \
      --key "${MACOS_NOTARY_KEY_FILE}" \
      --wait \
      --timeout "${TIMEOUT}"

  ###############################################################################
  # 11. Staple the notarization ticket
  ###############################################################################
  echo "Stapling notarization ticket."
  xcrun stapler staple "${APP_BUNDLE}"

  ###############################################################################
  # 12. Final verification
  ###############################################################################
  echo "Final verification."
  spctl --assess --type execute --verbose=2 "${APP_BUNDLE}"
  codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}"

  # Cleanup ZIP
  rm -f "${NOTARIZE_ZIP}"

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