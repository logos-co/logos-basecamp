#!/usr/bin/env bash
set -eo pipefail

function must_get_env() {
    declare -n VAR_VALUE="$1"
    [[ -z "${VAR_VALUE}" ]] && { echo "Missing env variable: ${1}" >&2; exit 1; }
}

must_get_env SM_API_KEY
must_get_env SM_CLIENT_CERT_PASSWORD
must_get_env SM_CLIENT_CERT_FILE
must_get_env WINDOWS_DIGICERT_KEYPAIR_ALIAS

export SM_HOST='https://clientauth.one.digicert.com'

SMCTL='/c/Program Files/DigiCert/DigiCert One Signing Manager Tools/smctl.exe'
SIGNTOOL=$(find '/c/Program Files (x86)/Windows Kits' -iname 'signtool.exe' | grep x64 | sort | tail -n1)

ZIP=$(find pkg -name '*.zip' | head -n1)
[[ -z "${ZIP}" ]] && { echo "No zip found in pkg/" >&2; exit 1; }

echo "Processing: ${ZIP}"

EXTRACT_DIR="pkg/signing-tmp"
rm -rf "${EXTRACT_DIR}"
mkdir -p "${EXTRACT_DIR}"
unzip -q -o "${ZIP}" -d "${EXTRACT_DIR}"
cmd //c "attrib -R pkg\\signing-tmp\\*.* /S"

SIGNED=()
while IFS= read -r FILE; do
    "${SIGNTOOL}" verify -pa "${FILE}" &>/dev/null && continue
    echo "Signing: ${FILE}"
    "${SMCTL}" sign --keypair-alias "${WINDOWS_DIGICERT_KEYPAIR_ALIAS}" --input "${FILE}" --simple
    SIGNED+=("${FILE}")
done < <(find "${EXTRACT_DIR}" -type f -iname '*.exe')

[[ ${#SIGNED[@]} -eq 0 ]] && { echo "No files needed signing"; exit 0; }

"${SIGNTOOL}" verify -pa "${SIGNED[0]}"

rm -f "${ZIP}"
powershell -Command "Compress-Archive -Path '$(cygpath -w "${EXTRACT_DIR}")\\*' -DestinationPath '$(cygpath -w "${ZIP}")' -Force"
rm -rf "${EXTRACT_DIR}"

echo "SUCCESS: ${#SIGNED[@]} files signed"