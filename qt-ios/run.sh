#!/bin/bash

# Qt iOS Hello World - Run Script
# This script runs the Qt app in the iOS Simulator

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-ios-sim"
APP_PATH="${BUILD_DIR}/Debug-iphonesimulator/Logos.app"

echo "=== Qt iOS Hello World Run Script ==="

# Check if app exists
if [ ! -d "${APP_PATH}" ]; then
    echo "Error: App not found at ${APP_PATH}"
    echo "Please run ./build.sh first"
    exit 1
fi

# Get available simulators
echo "Finding available iPhone simulators..."

# Get the device UDID for an iPhone simulator (prefer iPhone 16 or 15 if available)
SIMULATOR_UDID=$(xcrun simctl list devices available | grep -E "iPhone (16|15|14)" | head -1 | grep -oE "[A-F0-9-]{36}")

if [ -z "${SIMULATOR_UDID}" ]; then
    # Try getting any iPhone
    SIMULATOR_UDID=$(xcrun simctl list devices available | grep "iPhone" | head -1 | grep -oE "[A-F0-9-]{36}")
fi

if [ -z "${SIMULATOR_UDID}" ]; then
    echo "No iPhone simulator found. Available simulators:"
    xcrun simctl list devices available
    exit 1
fi

SIMULATOR_NAME=$(xcrun simctl list devices available | grep "${SIMULATOR_UDID}" | sed 's/ (.*//g' | xargs)
echo "Using simulator: ${SIMULATOR_NAME} (${SIMULATOR_UDID})"

# Boot the simulator if needed
echo "Booting simulator..."
xcrun simctl boot "${SIMULATOR_UDID}" 2>/dev/null || true

# Open the Simulator app
echo "Opening Simulator app..."
open -a Simulator

# Wait a moment for simulator to be ready
echo "Waiting for simulator to be ready..."
sleep 3

# Install the app
echo "Installing app..."
xcrun simctl install "${SIMULATOR_UDID}" "${APP_PATH}"

# Launch the app
echo "Launching app..."
BUNDLE_ID="com.example.qthelloworld"
xcrun simctl launch "${SIMULATOR_UDID}" "${BUNDLE_ID}"

echo ""
echo "=== App launched successfully! ==="
echo "The app should now be visible in the iOS Simulator."
echo ""
echo "Streaming logs (Ctrl+C to stop)..."
echo "========================================"

# Stream logs in real time
xcrun simctl spawn "${SIMULATOR_UDID}" log stream \
    --predicate 'process == "Logos"' \
    --level debug
