#!/bin/bash

# Qt iOS Hello World with liblogos - Build Script
# This script builds liblogos and modules for iOS and then builds the Qt app
#
# This script expects to be run from within a nix develop shell,
# which sets the required environment variables.
#
# For iOS builds, we use the _SRC variables (source directories from Nix store)
# and create local build directories for the iOS-compiled artifacts.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Local build cache directory (writable, outside Nix store)
IOS_BUILD_CACHE="${SCRIPT_DIR}/.ios-build-cache"

# Configuration
QT_IOS_PATH="${QT_IOS_PATH:-${HOME}/Qt6/6.8.2/ios}"
QT_HOST_PATH="${QT_HOST_PATH:-${HOME}/Qt6/6.8.2/macos}"
TARGET="${TARGET:-sim}"  # sim, device, or both

# For iOS builds, we need the source directories (from Nix store, read-only)
# These are set by nix develop as _SRC variables
if [[ -z "$LOGOS_LIBLOGOS_SRC" ]]; then
    echo "Error: LOGOS_LIBLOGOS_SRC is not set." >&2
    echo "Please run this script from within 'nix develop'." >&2
    exit 1
fi

if [[ -z "$LOGOS_CPP_SDK_SRC" ]]; then
    echo "Error: LOGOS_CPP_SDK_SRC is not set." >&2
    echo "Please run this script from within 'nix develop'." >&2
    exit 1
fi

if [[ -z "$LOGOS_PACKAGE_MANAGER_SRC" ]]; then
    echo "Error: LOGOS_PACKAGE_MANAGER_SRC is not set." >&2
    echo "Please run this script from within 'nix develop'." >&2
    exit 1
fi

if [[ -z "$LOGOS_CAPABILITY_MODULE_SRC" ]]; then
    echo "Error: LOGOS_CAPABILITY_MODULE_SRC is not set." >&2
    echo "Please run this script from within 'nix develop'." >&2
    exit 1
fi

usage() {
    cat <<'USAGE'
Usage: build.sh [options]

Options:
  --target <sim|device>   Build target (default: sim)
  --clean                 Clean build directories before building
  --skip-deps             Skip building dependencies (liblogos and modules)
  -h, --help              Show this help text

Environment (set by nix develop):
  LOGOS_CPP_SDK_SRC            Path to logos-cpp-sdk source (required for iOS)
  LOGOS_LIBLOGOS_SRC           Path to logos-liblogos source (required for iOS)
  LOGOS_PACKAGE_MANAGER_SRC    Path to logos-package-manager source (required for iOS)
  LOGOS_CAPABILITY_MODULE_SRC  Path to logos-capability-module source (required for iOS)
  QT_IOS_PATH                  Qt iOS installation path (default: ~/Qt6/6.8.2/ios)
  QT_HOST_PATH                 Qt host (macOS) path (default: ~/Qt6/6.8.2/macos)
  TARGET                       Build target: sim or device
USAGE
}

CLEAN=0
SKIP_DEPS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)
            [[ $# -lt 2 ]] && { echo "Missing value for $1" >&2; exit 1; }
            TARGET="$2"
            shift 2
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --skip-deps)
            SKIP_DEPS=1
            shift
            ;;
        --skip-liblogos)
            # Backwards compatibility
            SKIP_DEPS=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

# Validate target
if [[ "$TARGET" != "sim" && "$TARGET" != "device" ]]; then
    echo "Error: Invalid target '$TARGET'. Must be 'sim' or 'device'." >&2
    exit 1
fi

# Set paths based on target
# Build directories are in local cache (writable), not inside Nix store sources
if [[ "$TARGET" == "sim" ]]; then
    LIBLOGOS_BUILD_DIR="${IOS_BUILD_CACHE}/liblogos/build-ios-sim"
    PACKAGE_MANAGER_BUILD_DIR="${IOS_BUILD_CACHE}/package-manager/build-ios-sim"
    CAPABILITY_MODULE_BUILD_DIR="${IOS_BUILD_CACHE}/capability-module/build-ios-sim"
    APP_BUILD_DIR="${SCRIPT_DIR}/build-ios-sim"
    SDK_NAME="iphonesimulator"
    ARCH="x86_64"
    echo "=== Building for iOS Simulator ==="
else
    LIBLOGOS_BUILD_DIR="${IOS_BUILD_CACHE}/liblogos/build-ios-device"
    PACKAGE_MANAGER_BUILD_DIR="${IOS_BUILD_CACHE}/package-manager/build-ios-device"
    CAPABILITY_MODULE_BUILD_DIR="${IOS_BUILD_CACHE}/capability-module/build-ios-device"
    APP_BUILD_DIR="${SCRIPT_DIR}/build-ios-device"
    SDK_NAME="iphoneos"
    ARCH="arm64"
    echo "=== Building for iOS Device ==="
fi

# Create build cache directories
mkdir -p "${IOS_BUILD_CACHE}/liblogos"
mkdir -p "${IOS_BUILD_CACHE}/package-manager"
mkdir -p "${IOS_BUILD_CACHE}/capability-module"

# Step 0: Run code generation using nix environment (before unsetting it)
echo "=== Step 0: Running Code Generation ==="
GENERATED_CODE_DIR="${SCRIPT_DIR}/generated_code"
CPP_GENERATOR_BUILD_DIR="${IOS_BUILD_CACHE}/cpp-generator"
CPP_GENERATOR="${CPP_GENERATOR_BUILD_DIR}/bin/logos-cpp-generator"
METADATA_JSON="${SCRIPT_DIR}/metadata.json"

if [[ ! -x "$CPP_GENERATOR" ]]; then
    echo "Building logos-cpp-generator for macOS host..."
    rm -rf "${CPP_GENERATOR_BUILD_DIR}"
    mkdir -p "${CPP_GENERATOR_BUILD_DIR}"
    
    cmake "${LOGOS_CPP_SDK_SRC}/cpp-generator" \
        -B "${CPP_GENERATOR_BUILD_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="${QT_HOST_PATH}"
    
    cmake --build "${CPP_GENERATOR_BUILD_DIR}"
    echo "logos-cpp-generator built successfully"
fi

if [[ -x "$CPP_GENERATOR" && -f "$METADATA_JSON" ]]; then
    echo "Running logos-cpp-generator with --general-only..."
    echo "  Generator: ${CPP_GENERATOR}"
    echo "  Metadata: ${METADATA_JSON}"
    echo "  Output: ${GENERATED_CODE_DIR}"
    
    rm -rf "${GENERATED_CODE_DIR}"
    mkdir -p "${GENERATED_CODE_DIR}"
    
    # Copy pre-generated module API files from nix builds
    if [[ -d "${LOGOS_PACKAGE_MANAGER_ROOT}/include" ]]; then
        echo "Copying package_manager API files from nix build..."
        cp -L "${LOGOS_PACKAGE_MANAGER_ROOT}/include"/*.h "${GENERATED_CODE_DIR}/" 2>/dev/null || true
        cp -L "${LOGOS_PACKAGE_MANAGER_ROOT}/include"/*.cpp "${GENERATED_CODE_DIR}/" 2>/dev/null || true
    fi
    
    "${CPP_GENERATOR}" --metadata "${METADATA_JSON}" --general-only --output-dir "${GENERATED_CODE_DIR}"
    
    echo "Generated files:"
    ls -la "${GENERATED_CODE_DIR}/"
else
    echo "Warning: Skipping code generation"
    if [[ ! -x "$CPP_GENERATOR" ]]; then
        echo "  - logos-cpp-generator not found at ${CPP_GENERATOR}"
    fi
    if [[ ! -f "$METADATA_JSON" ]]; then
        echo "  - metadata.json not found at ${METADATA_JSON}"
    fi
fi
echo ""

# Override nix environment completely for iOS builds
# This is critical: Nix sets many environment variables that interfere with iOS cross-compilation
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer

# Use Qt's bundled CMake, not nix's cmake
QT_CMAKE_BIN="${HOME}/Qt6/Tools/CMake/CMake.app/Contents/bin"
export PATH="${QT_CMAKE_BIN}:/usr/bin:/bin:/usr/sbin:/sbin:$DEVELOPER_DIR/usr/bin"

# Unset ALL nix-related environment variables that interfere with iOS builds
# Compiler variables
unset CC CXX AR LD NM RANLIB STRIP
unset NIX_CC NIX_CXX NIX_CFLAGS_COMPILE NIX_LDFLAGS

# CMake search path variables (these make CMake find nix packages)
unset CMAKE_PREFIX_PATH CMAKE_MODULE_PATH CMAKE_FIND_ROOT_PATH CMAKE_SYSTEM_PREFIX_PATH
unset CMAKE_INCLUDE_PATH CMAKE_LIBRARY_PATH
unset NIXPKGS_CMAKE_PREFIX_PATH

# Qt-specific variables (QT_ADDITIONAL_PACKAGES_PREFIX_PATH is the main culprit!)
unset QT_ADDITIONAL_PACKAGES_PREFIX_PATH
unset Qt6_DIR Qt6_ROOT QT_PLUGIN_PATH QT_DIR QTDIR
unset QMAKE QMAKEPATH

# Other nix variables that might interfere
unset PKG_CONFIG_PATH

SDK_PATH=$(/usr/bin/xcrun --sdk "${SDK_NAME}" --show-sdk-path)

echo "Qt iOS Path: ${QT_IOS_PATH}"
echo "Qt Host Path: ${QT_HOST_PATH}"
echo "SDK Source: ${LOGOS_CPP_SDK_SRC}"
echo "liblogos Source: ${LOGOS_LIBLOGOS_SRC}"
echo "Package Manager Source: ${LOGOS_PACKAGE_MANAGER_SRC}"
echo "Capability Module Source: ${LOGOS_CAPABILITY_MODULE_SRC}"
echo "liblogos Build Dir: ${LIBLOGOS_BUILD_DIR}"
echo "Package Manager Build Dir: ${PACKAGE_MANAGER_BUILD_DIR}"
echo "Capability Module Build Dir: ${CAPABILITY_MODULE_BUILD_DIR}"
echo "App Build Dir: ${APP_BUILD_DIR}"
echo ""

# Validate paths
if [[ ! -d "$QT_IOS_PATH" ]]; then
    echo "Error: Qt iOS installation not found at $QT_IOS_PATH" >&2
    exit 1
fi

if [[ ! -d "$QT_HOST_PATH" ]]; then
    echo "Error: Qt host installation not found at $QT_HOST_PATH" >&2
    exit 1
fi

if [[ ! -d "$LOGOS_CPP_SDK_SRC" ]]; then
    echo "Error: logos-cpp-sdk source not found at $LOGOS_CPP_SDK_SRC" >&2
    exit 1
fi

if [[ ! -d "$LOGOS_LIBLOGOS_SRC" ]]; then
    echo "Error: logos-liblogos source not found at $LOGOS_LIBLOGOS_SRC" >&2
    exit 1
fi

if [[ ! -d "$LOGOS_PACKAGE_MANAGER_SRC" ]]; then
    echo "Error: logos-package-manager source not found at $LOGOS_PACKAGE_MANAGER_SRC" >&2
    exit 1
fi

if [[ ! -d "$LOGOS_CAPABILITY_MODULE_SRC" ]]; then
    echo "Error: logos-capability-module source not found at $LOGOS_CAPABILITY_MODULE_SRC" >&2
    exit 1
fi

QT_CMAKE="${QT_IOS_PATH}/bin/qt-cmake"
if [[ ! -x "$QT_CMAKE" ]]; then
    echo "Error: qt-cmake not found at $QT_CMAKE" >&2
    exit 1
fi

# Helper function to build a dependency with CMake for iOS
build_ios_dependency() {
    local NAME="$1"
    local SRC_DIR="$2"
    local BUILD_DIR="$3"
    local EXTRA_CMAKE_ARGS="${4:-}"
    
    echo "Building ${NAME} for iOS..."
    echo "  Source: ${SRC_DIR}"
    echo "  Build:  ${BUILD_DIR}"
    
    # Clean if requested
    if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
        echo "  Cleaning ${BUILD_DIR}..."
        rm -rf "$BUILD_DIR"
    fi
    
    mkdir -p "$BUILD_DIR"
    
    # Configure with CMake
    "${QT_CMAKE}" "${SRC_DIR}" \
        -B "${BUILD_DIR}" \
        -G Xcode \
        -DCMAKE_PREFIX_PATH="${QT_IOS_PATH}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_OSX_SYSROOT="${SDK_PATH}" \
        -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
        -DCMAKE_INSTALL_PREFIX="${BUILD_DIR}/install" \
        ${EXTRA_CMAKE_ARGS}
    
    # Build
    cmake --build "${BUILD_DIR}" --config Debug -- -sdk "${SDK_NAME}" -arch "${ARCH}"
    
    # Install to local prefix
    cmake --install "${BUILD_DIR}" --config Debug --prefix "${BUILD_DIR}/install" 2>/dev/null || true
    
    echo "  Done building ${NAME}"
    echo ""
}

# Step 1: Build dependencies for iOS
if [[ $SKIP_DEPS -eq 0 ]]; then
    echo "=== Step 1a: Building liblogos ==="
    build_ios_dependency "liblogos" \
        "${LOGOS_LIBLOGOS_SRC}" \
        "${LIBLOGOS_BUILD_DIR}" \
        "-DLOGOS_CPP_SDK_ROOT=${LOGOS_CPP_SDK_SRC}"
    
    # Use the install prefix for downstream dependencies (has include/ and lib/ layout)
    LIBLOGOS_INSTALL_DIR="${LIBLOGOS_BUILD_DIR}/install"
    
    echo "=== Step 1b: Building package_manager ==="
    build_ios_dependency "package_manager" \
        "${LOGOS_PACKAGE_MANAGER_SRC}" \
        "${PACKAGE_MANAGER_BUILD_DIR}" \
        "-DLOGOS_CPP_SDK_ROOT=${LOGOS_CPP_SDK_SRC} -DLOGOS_LIBLOGOS_ROOT=${LIBLOGOS_INSTALL_DIR}"
    
    echo "=== Step 1c: Building capability_module ==="
    build_ios_dependency "capability_module" \
        "${LOGOS_CAPABILITY_MODULE_SRC}" \
        "${CAPABILITY_MODULE_BUILD_DIR}" \
        "-DLOGOS_CPP_SDK_ROOT=${LOGOS_CPP_SDK_SRC} -DLOGOS_LIBLOGOS_ROOT=${LIBLOGOS_INSTALL_DIR}"
else
    echo "=== Skipping dependencies build (--skip-deps) ==="
    if [[ ! -d "${LIBLOGOS_BUILD_DIR}" ]]; then
        echo "Error: liblogos build not found at ${LIBLOGOS_BUILD_DIR}" >&2
        echo "Run without --skip-deps to build it first." >&2
        exit 1
    fi
    LIBLOGOS_INSTALL_DIR="${LIBLOGOS_BUILD_DIR}/install"
fi

# Step 2: Build Qt app with liblogos and modules
echo "=== Step 2: Building Qt App ==="

# Clean if requested
if [[ $CLEAN -eq 1 && -d "$APP_BUILD_DIR" ]]; then
    echo "Cleaning ${APP_BUILD_DIR}..."
    rm -rf "$APP_BUILD_DIR"
fi

mkdir -p "${APP_BUILD_DIR}"
cd "${APP_BUILD_DIR}"

# Configure with CMake for iOS using Qt's cmake wrapper
# Set CMAKE_PREFIX_PATH to ensure iOS Qt is found, not nix Qt
echo "Configuring CMake..."
echo "SDK Path: ${SDK_PATH}"
"${QT_CMAKE}" "${SCRIPT_DIR}" \
    -G Xcode \
    -DCMAKE_PREFIX_PATH="${QT_IOS_PATH}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_OSX_SYSROOT="${SDK_PATH}" \
    -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
    -DLIBLOGOS_ROOT="${LIBLOGOS_INSTALL_DIR}" \
    -DLOGOS_CPP_SDK_ROOT="${LOGOS_CPP_SDK_SRC}" \
    -DLOGOS_PACKAGE_MANAGER_ROOT="${PACKAGE_MANAGER_BUILD_DIR}/install" \
    -DLOGOS_CAPABILITY_MODULE_ROOT="${CAPABILITY_MODULE_BUILD_DIR}/install"

# Build using Xcode for the specific SDK/arch
echo ""
echo "Building..."
cmake --build . --config Debug -- -sdk "${SDK_NAME}" -arch "${ARCH}"

echo ""
echo "=== Build Complete ==="
echo "App bundle location: ${APP_BUILD_DIR}/Debug-${SDK_NAME}/Logos.app"

# Provide run instructions
if [[ "$TARGET" == "sim" ]]; then
    echo ""
    echo "To run on simulator:"
    echo "  1. Open Simulator.app"
    echo "  2. Run: xcrun simctl install booted '${APP_BUILD_DIR}/Debug-${SDK_NAME}/Logos.app'"
    echo "  3. Run: xcrun simctl launch booted com.example.qthelloworld"
fi
