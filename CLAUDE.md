# Logos Basecamp

A Qt/QML desktop application with a plugin-based architecture. It uses Nix for builds and has an MCP-based QML inspector for UI automation.

## Building & Running

```bash
# Build the app
nix build

# Run in dev mode (hot-reload QML, no disk cache)
./run-dev.sh

# Build + run directly
nix build && ./result/bin/logos-basecamp
```

## Testing

```bash
# Smoke test (validates app starts without QML errors)
nix build .#smoke-test -L

# Build test framework (one-time, rebuilds when logos-qt-mcp changes)
nix build .#logos-qt-mcp -o result-mcp

# UI integration tests (app must be running first)
node tests/ui-tests.mjs

# UI integration tests headless (CI mode)
node tests/ui-tests.mjs --ci ./result/bin/logos-basecamp

# Hermetic CI test via Nix
nix build .#integration-test -L
```

## App Structure

- **Sidebar** (left): Contains app plugin icons (top/middle) and system buttons at the bottom (Dashboard, Modules, Settings)
- **Plugins** appear as sidebar icons: `counter`, `counter_qml`, `package_manager_ui`, `webview_app`
- Plugins are loaded from `~/Library/Application Support/Logos/LogosBasecampDev/plugins/`
- Main UI is in `src/qml/`, with panels in `src/qml/panels/`

## QML Inspector (MCP)

Build the logos-qt-mcp package (one-time, includes MCP server + test framework):
```bash
nix build .#logos-qt-mcp -o result-mcp
```

The app runs an inspector server (default: localhost:3768) that the `qml-inspector` MCP tools connect to.

**Prefer high-level tools over tree exploration:**
- Use `qml_find_and_click({text: "..."})` to click buttons, tabs, sidebar items, etc. It supports partial, case-insensitive matching — e.g., `find_and_click({text: "package"})` will find "package_manager_ui".
- Use `qml_find_by_type` and `qml_find_by_property` to locate elements by type or property.
- Use `qml_list_interactive` to get an overview of all clickable/interactive elements (buttons, inputs, delegates) in the current UI state — great for figuring out what's available without exploring the tree.
- Use `qml_screenshot` to see the current state of the app.
- Only fall back to `qml_get_tree` if the above tools can't find what you need or you need to understand the full UI structure.

## Key Directories

- `src/qml/` - QML UI source files
- `src/qml/panels/` - Panel components (e.g., SidebarPanel.qml)
- `nix/` - Nix build configurations (app.nix, main-ui.nix, smoke-test.nix, integration-test.nix)
- `logos-qt-mcp` - QML Inspector: MCP server, test framework, Qt plugin (separate repo, flake input)
- `tests/` - UI integration tests
- `qt-ios/` - iOS build scripts
