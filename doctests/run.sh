#!/usr/bin/env bash
#
# Execute the logos-basecamp doc-tests end-to-end and regenerate their Markdown.
#
# Specs (both drive the SAME Modules-view walk; they differ only in what they
# build — the dev app vs. the shippable bundle):
#   basecamp-modules.test.yaml — builds THIS basecamp commit as the normal
#       development app (`#app`, inspector-enabled by default), launches the dev
#       binary headless, and drives its Modules view: it opens the UI Modules tab
#       to see the installed apps (UI plugins), switches to the Core Modules tab
#       to see the loaded runtime modules with their stats, and opens an installed
#       app from the sidebar, capturing screenshots of each step into
#       outputs/images/ (screenshots prefixed `basecamp-`).
#   basecamp-modules-bundle.test.yaml — the bundle twin: builds THIS commit as the
#       real portable bundle (inspector-enabled `#bin-bundle-dir-inspector`),
#       launches the bundle binary headless, and drives the identical Modules-view
#       walk, capturing screenshots prefixed `bundle-` (so the two specs don't
#       collide in outputs/images/).
#
# The runner is the shared `doctest` CLI
# (https://github.com/logos-co/logos-doctest), invoked directly via its flake.
# Each spec runs into ./outputs/ via --output-dir (so ui_test screenshots land in
# outputs/images/ next to the generated .md); `doctest generate` renders the .md;
# `doctest clean` then strips build artifacts, keeping only the .md and images/.
#
# To run against a local logos-doctest checkout instead of the published flake,
# set DOCTEST, e.g.:  DOCTEST="nix run path:../../logos-doctest --" ./run.sh
#
# To validate a local logos-qt-mcp change (e.g. the inspector command timeout)
# before it is published, point the specs' qt-mcp build at a local checkout:
#   QT_MCP_FLAKE=path:/abs/path/to/logos-qt-mcp ./run.sh
# Unset, the specs build the published flake (github:logos-co/logos-qt-mcp).
#
set -euo pipefail

# Run from this doctests/ directory regardless of where the script is invoked from.
cd "$(dirname "$0")"

# The doctest CLI. Override by exporting DOCTEST (space-separated command).
# If a sibling logos-doctest checkout exists in the workspace, prefer it so
# locally-added runner features (e.g. the `call_method` ui_test action) are
# picked up without a push; otherwise fall back to the published flake (CI).
DEFAULT_DOCTEST="nix run github:logos-co/logos-doctest --"
LOCAL_DOCTEST="../../logos-doctest"
if [ -z "${DOCTEST:-}" ] && [ -f "${LOCAL_DOCTEST}/flake.nix" ]; then
  DEFAULT_DOCTEST="nix run path:${LOCAL_DOCTEST} --"
  echo "==> Using local logos-doctest checkout (${LOCAL_DOCTEST})"
fi
read -r -a DOCTEST <<< "${DOCTEST:-$DEFAULT_DOCTEST}"
OUTPUT_DIR="./outputs"

# Build the doc-tests against THIS repo's current commit rather than the latest
# published flake. Each spec pins `github:logos-co/logos-basecamp{release}` to
# $COMMIT via --release-for, so the bundle spec builds exactly what's checked out
# here and the UI spec launches the same commit. Override by exporting COMMIT
# (e.g. a tag), or set COMMIT="" to fall back to latest master.
#
# Note: nix fetches the commit from the GitHub remote, so $COMMIT must be pushed
# to logos-co/logos-basecamp. A local-only / uncommitted HEAD won't resolve;
# export COMMIT="" (or push first) in that case.
COMMIT="${COMMIT-$(git rev-parse HEAD)}"
RELEASE_FOR=()
if [ -n "${COMMIT}" ]; then
  RELEASE_FOR=(--release-for "logos-basecamp=${COMMIT}")
  echo "==> Pinning logos-basecamp to ${COMMIT}"
else
  echo "==> COMMIT empty; building from latest logos-basecamp master"
fi

echo "==> Clearing previous ${OUTPUT_DIR}/"
# A prior run copies artifacts out of the read-only nix store, so the
# directories land read-only (r-x) too. `rm -rf` can't delete files inside a
# directory it can't write to, so restore write permission first.
if [ -e "${OUTPUT_DIR}" ]; then
  chmod -R u+w "${OUTPUT_DIR}" 2>/dev/null || true
fi
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

# Run each spec into ./outputs/ separately. --output-dir is single-spec, but
# passing it once per spec makes the runner write each spec's ui_test
# screenshots into outputs/images/ (beside the generated .md). The bundle spec
# has no screenshots; the package-manager spec populates outputs/images/.
for spec in *.test.yaml; do
  name="$(basename "${spec%.test.yaml}")"
  echo "==> Running ${spec} into ${OUTPUT_DIR}/"
  # ${RELEASE_FOR[@]+...} guards the expansion so an empty array doesn't trip
  # `set -u` on older bash (e.g. macOS's stock 3.2).
  "${DOCTEST[@]}" run "${spec}" \
    --verbose \
    --continue-on-fail \
    ${RELEASE_FOR[@]+"${RELEASE_FOR[@]}"} \
    --output-dir "${OUTPUT_DIR}/"

  echo "==> Generating ${OUTPUT_DIR}/${name}.md"
  "${DOCTEST[@]}" generate "${spec}" \
    ${RELEASE_FOR[@]+"${RELEASE_FOR[@]}"} \
    -o "${OUTPUT_DIR}/${name}.md"
done

echo "==> Cleaning build artifacts from ${OUTPUT_DIR}/ (keeps .md and images/)"
"${DOCTEST[@]}" clean "${OUTPUT_DIR}" --verbose

echo "==> Done. Rendered docs and screenshots are in ${OUTPUT_DIR}/"
