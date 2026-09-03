#!/usr/bin/env bash
# Build and run the mock-backend tests without any nix or CMake wiring.
#
# logos-basecamp has no top-level CMakeLists.txt — app/ and src/ are separate
# CMake projects driven by nix/app.nix and nix/main-ui.nix. Until a mobile
# derivation exists to pull in mock/CMakeLists.txt, this script is how you
# run these tests. It needs only Qt6 + a C++17 compiler.
#
#   ./mock/tests/run-standalone.sh          # both suites
#   ./mock/tests/run-standalone.sh --asan   # under AddressSanitizer+UBSan
#
# Requires: Qt6 Core dev packages. On Debian/Ubuntu:
#   apt-get install qt6-base-dev qt6-base-dev-tools nlohmann-json3-dev

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MOCKDIR="$(dirname "$HERE")"
BASECAMP="$(dirname "$MOCKDIR")"
REPOS="$(dirname "$BASECAMP")"
BUILD="${TMPDIR:-/tmp}/logos-mock-tests"

SAN=""
if [[ "${1:-}" == "--asan" ]]; then
    SAN="-fsanitize=address,undefined -g"
fi

# --- locate Qt tooling -------------------------------------------------------
#
# moc, rcc and repc MUST come from the same Qt as the headers we compile
# against. Getting this wrong is not a subtle failure but it is a confusing
# one: moc emits a #error ("generated using the moc from X, cannot be used
# with the include files from this version of Qt") buried under a hundred
# lines of follow-on syntax errors about QtPrivate::TypeAndForceComplete.
#
# So resolve them from pkg-config's host_bins — the directory Qt itself
# declares its tools live in — rather than from PATH or a guessed prefix.
# Qt installs moc under libexec/, not bin/, so a bare `command -v moc` misses
# it and happily finds a DIFFERENT Qt's moc from the system path instead.
#
# MOC/RCC/REPC can be set explicitly to override (nix/mock-tests.nix does).
QT_HOST_BINS="$(pkg-config --variable=host_bins Qt6Core 2>/dev/null || true)"
QT_LIBEXEC="$(pkg-config --variable=libexecdir Qt6Core 2>/dev/null || true)"

find_qt_tool() {
    local tool="$1" override="$2"
    if [[ -n "$override" ]]; then echo "$override"; return; fi
    for d in "$QT_LIBEXEC" "$QT_HOST_BINS"; do
        [[ -n "$d" && -x "$d/$tool" ]] && { echo "$d/$tool"; return; }
    done
    command -v "$tool" 2>/dev/null && return
    echo "ERROR: could not locate Qt tool '$tool'. Set ${tool^^}=/path/to/$tool." >&2
    exit 1
}

RCC="$(find_qt_tool rcc  "${RCC:-}")"
MOC="$(find_qt_tool moc  "${MOC:-}")"

# Fail loudly on a mismatch rather than letting moc's #error do it 100 lines later.
QT_HDR_VER="$(pkg-config --modversion Qt6Core 2>/dev/null || echo unknown)"
QT_MOC_VER="$("$MOC" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | tail -1 || echo unknown)"
if [[ "$QT_HDR_VER" != "unknown" && "$QT_MOC_VER" != "unknown" \
      && "${QT_HDR_VER%.*}" != "${QT_MOC_VER%.*}" ]]; then
    echo "ERROR: moc is Qt $QT_MOC_VER but the headers are Qt $QT_HDR_VER ($MOC)." >&2
    echo "       Set MOC/RCC/REPC to the matching Qt's tools." >&2
    exit 1
fi
echo "Using Qt $QT_HDR_VER (moc: $MOC)"

QT_CORE_CFLAGS="$(pkg-config --cflags Qt6Core)"
QT_CORE_LIBS="$(pkg-config --libs Qt6Core)"

INCLUDES=(
    -I"$MOCKDIR/src"
    -I"$BASECAMP/app/utils"
)

mkdir -p "$BUILD"
cd "$BUILD"

# NOTE: the build dir must be executable. Some containers mount /tmp noexec —
# set TMPDIR to somewhere else if the binaries refuse to run.

# --- fixture as a Qt resource ------------------------------------------------
cp "$MOCKDIR/fixtures/mock-backend.json" .
cat > fixture.qrc <<'EOF'
<RCC><qresource prefix="/mock"><file>mock-backend.json</file></qresource></RCC>
EOF
"$RCC" fixture.qrc -o qrc_fixture.cpp

FAILED=0
run_suite() {
    local name="$1"; shift
    echo "── $name ──"
    if "$@"; then
        echo "   PASS"
    else
        echo "   FAIL"
        FAILED=1
    fi
    echo
}

# --- fixture resolution ----------------------------------------
#
# MockStore and TokenManager come from logos-protocol. Prefer linking the
# PREBUILT library when one is available (LOGOS_PROTOCOL_LIB): it is what
# Basecamp actually links, and compiling those sources here instead means
# running moc ourselves, which breaks the moment the local moc and the Qt
# headers come from different Qt versions — moc emits
# QtPrivate::TypeAndForceComplete on newer Qt and older headers reject it.
#
# The from-source path stays as a fallback for a bare checkout with no nix.
# shellcheck disable=SC2086
g++ -std=c++17 -fPIC $SAN -o mock_fixture_test \
    "$HERE/mock_fixture_test.cpp" "$MOCKDIR/src/MockBackendFixture.cpp" qrc_fixture.cpp \
    "${INCLUDES[@]}" $QT_CORE_CFLAGS $QT_CORE_LIBS
run_suite "fixture resolution" env HOME="$BUILD" LOGOS_USER_DIR="$BUILD/userdir" ./mock_fixture_test

if [[ $FAILED -eq 0 ]]; then
    echo "All suites passed."
else
    echo "One or more suites FAILED."
fi
exit $FAILED
