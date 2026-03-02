#!/usr/bin/env bash
set -euo pipefail

###############################################################################
# macOS Code Signing & Notarization for LogosApp.app
#
# Required env vars:
#   MACOS_CODESIGN_IDENT       - Developer ID Application identity
#   MACOS_NOTARY_ISSUER_ID     - App Store Connect API issuer ID
#   MACOS_NOTARY_KEY_ID        - App Store Connect API key ID
#   MACOS_NOTARY_KEY_FILE      - Path to .p8 AuthKey file
#   MACOS_KEYCHAIN_PASS        - Password for the .p12 certificate
#   MACOS_KEYCHAIN_FILE        - Path to the .p12 certificate file
###############################################################################

APP_BUNDLE="./LogosApp.app"
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
# 2. Strip extended attributes & clean up unsignable files
###############################################################################
echo "==> Stripping extended attributes..."
xattr -cr "${APP_BUNDLE}"

echo "==> Removing static libraries..."
find "${APP_BUNDLE}" -name "*.a" -delete

###############################################################################
# 3. Move Qt resources out of Frameworks/ into Resources/
#    Frameworks/ is deep-inspected by codesign — text files, QML, images etc.
#    in there cause "code object is not signed" errors.
###############################################################################
echo "==> Moving Qt resources out of Frameworks..."
mkdir -p "${CONTENTS}/Resources"
if [[ -d "${CONTENTS}/Frameworks/qt" ]]; then
    mv "${CONTENTS}/Frameworks/qt" "${CONTENTS}/Resources/qt"
fi
# Move Logos QML modules into the qt/qml tree
if [[ -d "${CONTENTS}/Frameworks/Logos" ]]; then
    # Remove the old symlink-based Logos dir that came with qt/
    rm -rf "${CONTENTS}/Resources/qt/qml/Logos"
    # Move the actual Logos modules into place
    mv "${CONTENTS}/Frameworks/Logos" "${CONTENTS}/Resources/qt/qml/Logos"
fi

echo "==> Cleaning up broken symlinks..."
# Remove broken symlinks left over from the Logos move
find "${CONTENTS}/Resources" -type l ! -exec test -e {} \; -delete 2>/dev/null || true

echo "==> Fixing @loader_path references in Resources/qt plugins..."
find "${CONTENTS}/Resources/qt" -name "*.dylib" -type f | while read -r dylib; do
    # Get all @loader_path dependencies
    otool -L "$dylib" | grep '@loader_path' | awk '{print $1}' | while read -r dep; do
        lib_name=$(basename "$dep")
        # Calculate correct relative path from this dylib's location to Frameworks/
        dylib_dir=$(dirname "$dylib")
        rel_path=$(python3 -c "import os; print(os.path.relpath('${CONTENTS}/Frameworks', '$dylib_dir'))")
        new_path="@loader_path/${rel_path}/${lib_name}"
        install_name_tool -change "$dep" "$new_path" "$dylib" 2>/dev/null || true
    done
done

###############################################################################
# 4. Fix Qt framework bundle structure
#    Qt frameworks ship without Resources/Info.plist which makes codesign
#    reject them as "bundle format unrecognized". Add a minimal Info.plist
#    and the required Resources symlink.
###############################################################################
echo "==> Fixing Qt framework bundle structure..."
for fw in "${CONTENTS}/Frameworks/"*.framework; do
    name=$(basename "$fw" .framework)
    resources="$fw/Versions/A/Resources"
    mkdir -p "$resources"
    cat > "$resources/Info.plist" <<FWEOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>
    <string>org.qt-project.${name}</string>
    <key>CFBundleName</key>
    <string>${name}</string>
    <key>CFBundlePackageType</key>
    <string>FMWK</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
</dict>
</plist>
FWEOF
    if [[ ! -e "$fw/Resources" ]]; then
        ln -s "Versions/Current/Resources" "$fw/Resources"
    fi
done

###############################################################################
# 5. Update the shell wrapper to use Resources/qt instead of Frameworks/qt
###############################################################################
echo "==> Updating LogosApp wrapper script..."
cat > "${CONTENTS}/MacOS/LogosApp" <<'WRAPPER'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export QT_PLUGIN_PATH="$DIR/../Resources/qt/plugins"
export QML2_IMPORT_PATH="$DIR/../Resources/qt/qml"
exec "$DIR/LogosApp.bin" "$@"
WRAPPER
chmod +x "${CONTENTS}/MacOS/LogosApp"

###############################################################################
# Sign + repack .lgx archives in Contents/preinstall/
###############################################################################
echo "==> Signing dylibs inside .lgx archives..."
find "${CONTENTS}/preinstall" -name "*.lgx" | while read -r lgx; do
    echo "  Processing: ${lgx}"
    tmpdir=$(mktemp -d)
    tar -xzf "$lgx" -C "$tmpdir"
    find "$tmpdir" -name "*.dylib" -exec \
        codesign --force --options runtime --sign "${MACOS_CODESIGN_IDENT}" --timestamp {} \;
    gtar -czf "$lgx" -C "$tmpdir" --transform 's|^\./||' .
    rm -rf "$tmpdir"
    echo "  Repacked: ${lgx}"
done

###############################################################################
# 6. Sign all dylibs in Frameworks/
###############################################################################
echo "==> Signing dylibs in Frameworks..."
find "${CONTENTS}/Frameworks" -name '*.dylib' -type f | while read -r dylib; do
    echo "  Signing: ${dylib}"
    codesign "${CODESIGN_OPTS[@]}" "${dylib}"
done

###############################################################################
# 7. Sign all dylibs in Resources/qt/
###############################################################################
echo "==> Signing dylibs in Resources/qt..."
find "${CONTENTS}/Resources/qt" -type f -name '*.dylib' | while read -r dylib; do
    echo "  Signing: ${dylib}"
    codesign "${CODESIGN_OPTS[@]}" "${dylib}"
done

###############################################################################
# 8. Sign Qt frameworks (binary inside Versions/A/ then the .framework dir)
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
# 9. Sign executables in Contents/MacOS/
###############################################################################
echo "==> Signing executables..."
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
echo "  Signing wrapper: ${CONTENTS}/MacOS/LogosApp"
codesign --force --timestamp --sign "${MACOS_CODESIGN_IDENT}" "${CONTENTS}/MacOS/LogosApp"

###############################################################################
# 10. Sign the top-level app bundle
###############################################################################
echo "==> Signing app bundle..."
codesign "${CODESIGN_OPTS[@]}" --entitlements "${ENTITLEMENTS}" "${APP_BUNDLE}"

###############################################################################
# 11. Verify
###############################################################################
echo "==> Verifying signature..."
codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}"

echo "==> Checking Gatekeeper assessment..."
spctl --assess --type execute --verbose=2 "${APP_BUNDLE}" || true

###############################################################################
# 12. Create ZIP for notarization
###############################################################################
echo "==> Creating ZIP for notarization..."
NOTARIZE_ZIP="LogosApp.zip"
ditto -c -k --keepParent "${APP_BUNDLE}" "${NOTARIZE_ZIP}"

###############################################################################
# 13. Submit for notarization
###############################################################################
echo "==> Submitting for notarization..."
xcrun notarytool submit "${NOTARIZE_ZIP}" \
    --issuer "${MACOS_NOTARY_ISSUER_ID}" \
    --key-id "${MACOS_NOTARY_KEY_ID}" \
    --key "${MACOS_NOTARY_KEY_FILE}" \
    --wait \
    --timeout 30m

###############################################################################
# 14. Staple the notarization ticket
###############################################################################
echo "==> Stapling notarization ticket..."
xcrun stapler staple "${APP_BUNDLE}"

###############################################################################
# 15. Final verification
###############################################################################
echo "==> Final verification..."
spctl --assess --type execute --verbose=2 "${APP_BUNDLE}"
codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}"

###############################################################################
# 16. Cleanup
###############################################################################
echo "==> Cleaning up keychain..."
security delete-keychain "${KEYCHAIN_NAME}"
rm -f "${ENTITLEMENTS}"

echo "==> Done! ${APP_BUNDLE} is signed and notarized."