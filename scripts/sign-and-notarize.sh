#!/usr/bin/env bash
set -euo pipefail

###############################################################################
# macOS Code Signing & Notarization for LogosBasecamp.app
#
# Required env vars:
#   MACOS_CODESIGN_IDENT       - Developer ID Application identity
#   MACOS_NOTARY_ISSUER_ID     - App Store Connect API issuer ID
#   MACOS_NOTARY_KEY_ID        - App Store Connect API key ID
#   MACOS_NOTARY_KEY_FILE      - Path to .p8 AuthKey file
#   MACOS_KEYCHAIN_PASS        - Password for the .p12 certificate
#   MACOS_KEYCHAIN_FILE        - Path to the .p12 certificate file
###############################################################################

APP_BUNDLE="./LogosBasecamp.app"
CONTENTS="${APP_BUNDLE}/Contents"
ENTITLEMENTS="entitlements.plist"
KEYCHAIN_NAME="build.keychain"

# Codesign options — hardened runtime required for notarization
CODESIGN_OPTS=(
    --force
    --timestamp
    --options runtime
    --sign "${MACOS_CODESIGN_IDENT}"
)

###############################################################################
# 0. Create entitlements file (adjust as needed for your app)
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
# 1. Set up a temporary keychain and import the certificate
###############################################################################
echo "==> Setting up keychain..."
security delete-keychain "${KEYCHAIN_NAME}" 2>/dev/null || true
security create-keychain -p "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_NAME}"
security unlock-keychain -p "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_NAME}"

# Import cert
security import "${MACOS_KEYCHAIN_FILE}" \
    -k "${KEYCHAIN_NAME}" \
    -P "${MACOS_KEYCHAIN_PASS}" \
    -T /usr/bin/codesign \
    -T /usr/bin/security

security set-key-partition-list \
    -S apple-tool:,apple:,codesign: \
    -s -k "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_NAME}"

# CRITICAL: Explicitly include login + System keychains so Apple root certs are found
security list-keychains -d user -s \
    "${KEYCHAIN_NAME}" \
    "$HOME/Library/Keychains/login.keychain-db" \
    "/Library/Keychains/System.keychain"

echo "==> Keychain search list:"
security list-keychains
echo "==> Available identities:"
security find-identity -v -p codesigning

###############################################################################
# 2. Sign dylibs in pre-installed modules/ and plugins/ directories
###############################################################################
echo "==> Signing dylibs in modules/..."
find "${CONTENTS}/modules" -name '*.dylib' -type f 2>/dev/null | while read -r dylib; do
    echo "  Signing: ${dylib}"
    codesign --force --options runtime --sign "${MACOS_CODESIGN_IDENT}" --timestamp "${dylib}"
done

echo "==> Signing dylibs in plugins/..."
find "${CONTENTS}/plugins" -name '*.dylib' -type f 2>/dev/null | while read -r dylib; do
    echo "  Signing: ${dylib}"
    codesign --force --options runtime --sign "${MACOS_CODESIGN_IDENT}" --timestamp "${dylib}"
done

###############################################################################
# 3. Sign all dylibs in Frameworks/
###############################################################################
echo "==> Signing dylibs in Frameworks..."
find "${CONTENTS}/Frameworks" -name '*.dylib' -type f | while read -r dylib; do
    echo "  Signing: ${dylib}"
    codesign "${CODESIGN_OPTS[@]}" "${dylib}"
done

###############################################################################
# 4. Sign all dylibs in Resources/qt/
###############################################################################
echo "==> Signing dylibs in Resources/qt..."
find "${CONTENTS}/Resources/qt" -type f -name '*.dylib' | while read -r dylib; do
    echo "  Signing: ${dylib}"
    codesign "${CODESIGN_OPTS[@]}" "${dylib}"
done

###############################################################################
# 5. Sign Qt frameworks (binary inside Versions/A/ then the .framework dir)
###############################################################################
echo "==> Signing Qt frameworks..."
find "${CONTENTS}/Frameworks" -name '*.framework' -type d -maxdepth 1 | sort | while read -r fw; do
    fw_name=$(basename "${fw}" .framework)
    fw_binary="${fw}/Versions/A/${fw_name}"

    if [[ -f "${fw_binary}" ]]; then
        echo "  Signing framework binary: ${fw_binary}"
        codesign "${CODESIGN_OPTS[@]}" "${fw_binary}"
    fi

    echo "  Signing framework bundle: ${fw}"
    codesign "${CODESIGN_OPTS[@]}" "${fw}"
done

###############################################################################
# 6. Sign executables in Contents/MacOS/
###############################################################################
echo "==> Signing executables..."
# Sign all Mach-O binaries first (with entitlements)
for exe in "${CONTENTS}/MacOS/LogosBasecamp.bin" \
           "${CONTENTS}/MacOS/logos-basecamp" \
           "${CONTENTS}/MacOS/logos_host" \
           "${CONTENTS}/MacOS/logoscore"; do
    if [[ -f "${exe}" ]]; then
        echo "  Signing: ${exe}"
        codesign "${CODESIGN_OPTS[@]}" --entitlements "${ENTITLEMENTS}" "${exe}"
    fi
done

# Sign the shell script wrapper last (no --options runtime for scripts)
echo "  Signing wrapper: ${CONTENTS}/MacOS/LogosBasecamp"
codesign --force --timestamp --sign "${MACOS_CODESIGN_IDENT}" "${CONTENTS}/MacOS/LogosBasecamp"

###############################################################################
# 7. Sign the top-level app bundle
###############################################################################
echo "==> Signing app bundle..."
codesign "${CODESIGN_OPTS[@]}" --entitlements "${ENTITLEMENTS}" "${APP_BUNDLE}"

###############################################################################
# 8. Verify
###############################################################################
echo "==> Verifying signature..."
codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}"

echo "==> Checking Gatekeeper assessment..."
spctl --assess --type execute --verbose=2 "${APP_BUNDLE}" || true

###############################################################################
# 9. Create ZIP for notarization
###############################################################################
echo "==> Creating ZIP for notarization..."
NOTARIZE_ZIP="LogosBasecamp.zip"
ditto -c -k --keepParent "${APP_BUNDLE}" "${NOTARIZE_ZIP}"

###############################################################################
# 10. Submit for notarization
###############################################################################
echo "==> Submitting for notarization..."
xcrun notarytool submit "${NOTARIZE_ZIP}" \
    --issuer "${MACOS_NOTARY_ISSUER_ID}" \
    --key-id "${MACOS_NOTARY_KEY_ID}" \
    --key "${MACOS_NOTARY_KEY_FILE}" \
    --wait \
    --timeout 30m

###############################################################################
# 11. Staple the notarization ticket
###############################################################################
echo "==> Stapling notarization ticket..."
xcrun stapler staple "${APP_BUNDLE}"

###############################################################################
# 12. Final verification
###############################################################################
echo "==> Final verification..."
spctl --assess --type execute --verbose=2 "${APP_BUNDLE}"
codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}"

###############################################################################
# 13. Cleanup
###############################################################################
echo "==> Cleaning up keychain..."
security delete-keychain "${KEYCHAIN_NAME}"
rm -f "${ENTITLEMENTS}"

echo "==> Done! ${APP_BUNDLE} is signed and notarized."