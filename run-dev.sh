#!/bin/bash
# This allows you to edit QML files and see changes without rebuilding

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Set QML_UI to point to the main_ui QML source directory
export QML_UI="$SCRIPT_DIR/src"

# Ensure QML uses filesystem sources and skips disk cache
export QML_IMPORT_PATH="$QML_UI/qml:$QML_UI:$QML_IMPORT_PATH"
export QML2_IMPORT_PATH="$QML_UI/qml:$QML_UI:$QML2_IMPORT_PATH"
export QML_DISABLE_DISK_CACHE=1
export QML_NO_CACHE=1

# Auto-detect Wayland session (override with QT_QPA_PLATFORM=xcb to force X11)
if [ -n "$WAYLAND_DISPLAY" ] && [ -z "$QT_QPA_PLATFORM" ]; then
    export QT_QPA_PLATFORM=wayland
fi

# Add design system to import path if available
if [ -n "$LOGOS_DESIGN_SYSTEM_ROOT" ]; then
    # Use design system from environment variable (typically from nix shell)
    export QML2_IMPORT_PATH="$LOGOS_DESIGN_SYSTEM_ROOT/lib:$QML2_IMPORT_PATH"
    echo "Design system: $LOGOS_DESIGN_SYSTEM_ROOT"
elif [ -d "$SCRIPT_DIR/../logos-design-system/src/qml" ]; then
    # Fallback to local design system for development (if sibling directory exists)
    LOCAL_DS_PATH="$SCRIPT_DIR/../logos-design-system/src/qml"
    export QML2_IMPORT_PATH="$LOCAL_DS_PATH:$QML2_IMPORT_PATH"
    echo "Design system: $LOCAL_DS_PATH (local development)"
else
    # Use design system from the nix build's lib directory
    echo "Design system: (from nix build)"
fi

# Print the paths being used
echo "================================================"
echo "Starting Logos App in DEVELOPMENT mode"
echo "================================================"
echo "QML_UI path: $QML_UI"
echo ""
echo "QML files will be loaded from the filesystem."
echo "The QML_UI path is also added to the engine import path, so"
echo "nested components (e.g. SidebarIconButton) load from disk too."
echo "================================================"
echo ""

# Run the app from the nix result
if [ -f "./result/bin/LogosBasecamp" ]; then
    ./result/bin/LogosBasecamp "$@"
else
    echo "Error: Application binary not found in ./result/bin/"
    echo "Please build the app first with: nix build"
    exit 1
fi
