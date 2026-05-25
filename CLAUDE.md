# Logos Basecamp

A Qt/QML desktop application with a plugin-based architecture. It uses Nix for builds and has an MCP-based QML inspector for UI automation.

## Building & Running

```bash
# Build the app
nix build

# Run in dev mode (hot-reload QML, no disk cache)
./run-dev.sh

# Build + run directly
nix build && ./result/bin/LogosBasecamp
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
node tests/ui-tests.mjs --ci ./result/bin/LogosBasecamp

# Hermetic CI test via Nix
nix build .#integration-test -L
```

## App Structure

- **Sidebar** (left): Contains app plugin icons (top/middle) and system buttons at the bottom (Dashboard, Modules, Settings)
- **Plugins** appear as sidebar icons: `package_manager_ui`
- Plugins are loaded from `~/Library/Application Support/Logos/LogosBasecampDev/plugins/`
- Main UI is in `src/qml/`, with panels in `src/qml/panels/`

## C++ Architecture

The backend is split into four classes with a unidirectional dependency graph:

```
MainUIBackend (facade, QML-facing — owns the other three as Qt children)
    │
    ├─► CoreModuleManager    (wraps logos_core_* C API, stats polling)
    │       ▲
    │       │ (uses for all C API calls)
    ├─► UIPluginManager       (UI plugin widgets, app launcher, unload cascade)
    │       ▲
    │       │ (queries for installType / missing-deps / dependents;
    │       │  provides intersectWithLoaded / teardownUiPluginWidget)
    └─► PackageCoordinator    (package_manager IPC, install/uninstall/upgrade
                               orchestration, install & uninstall-cascade dialogs)
```

### MainUIBackend (`src/MainUIBackend.h/.cpp`)
Thin QML-facing facade. Holds only navigation state (`m_currentActiveSectionIndex`, `m_sections`). Every QML-visible slot/signal is a one-line delegation into one of the three managers. The `coreModules()` Q_PROPERTY is the one exception — it composes data from multiple managers (known list + stats from CoreModuleManager, installType from PackageCoordinator). The `cancelPendingAction(name)` slot fans out to both UIPluginManager and PackageCoordinator so the un-involved one no-ops.

### CoreModuleManager (`src/CoreModuleManager.h/.cpp`)
Single owner of the `logos_core_*` C API. Provides thin wrappers: `knownModules()`, `loadedModules()`, `loadModule()`, `unloadModule()`, `unloadModuleWithDependents()`, plus a stats timer that periodically queries `logos_core_get_module_stats`. Nothing else in the app calls the C API directly.

### UIPluginManager (`src/UIPluginManager.h/.cpp`)
Owns UI plugin widget lifecycle in-process: PluginLoader wiring, widget teardown, app launcher state, UI-plugin metadata cache (`m_uiPluginMetadata`) used for load dispatch. Runs the local *unload* cascade (no package_manager involvement). Queries PackageCoordinator for installType / missing-deps / dependents via accessor methods. Exposes `intersectWithLoaded(names)` + `teardownUiPluginWidget(name)` for PackageCoordinator to call during uninstall cascade.

- **Load/unload**: `loadUiModule`, `unloadUiModule`, `loadCoreModule`, `unloadCoreModule` — pre-flight dependency checks, then delegates to CoreModuleManager
- **Unload cascade**: `confirmUnloadCascade`, `cancelUnloadCascade` — single-slot `m_pendingUnload` drives the QML dialog
- **App launcher**: `activateApp`, `onAppLauncherClicked`, `onPluginWindowClosed`, `setCurrentVisibleApp`

### PackageCoordinator (`src/PackageCoordinator.h/.cpp`)
Owns every interaction with the `package_manager` LogosAPI module. (Named `PackageCoordinator` rather than `PackageManager` to avoid colliding with the SDK-generated `PackageManager` proxy class.) Event subscriptions, install/uninstall/upgrade IPC, the install-confirmation dialog, the uninstall-cascade dialog, plus the package-state caches (`m_installTypeByModule`, `m_missingDepsByModule`, `m_dependentsByModule`). Holds the gated-cascade pending slot for uninstall/upgrade ops.

- **Install from LGX**: `installPluginFromPath` → `inspectPackageAsync` → shows install-confirm dialog (fresh install or upgrade) → `confirmInstall()`/`cancelInstall()`
- **Gated uninstall/upgrade**: Subscribes to `package_manager` module's `beforeUninstall`/`beforeUpgrade` events, acks within 3s, shows cascade dialog, then confirms/cancels back to the module
- **Cascade confirmation**: `confirmUninstallCascade`, `cancelPendingAction` — drives cascade unload via CoreModuleManager + UIPluginManager, then hands back to the module
- **Metadata refresh**: `refresh()` triggers the full `getInstalledUiPlugins` + `getInstalledPackages` + per-package `resolveFlatDependencies/Dependents` chain; pushes UI metadata to UIPluginManager via `uiPluginsFetched` signal

### Construction & Destruction Order
CoreModuleManager is constructed first, UIPluginManager second (receives CoreModuleManager), PackageCoordinator third (receives both). UIPluginManager's `setPackageCoordinator` is called after all three exist, closing the cycle and wiring the `uiPluginsFetched`/`uiModulesChanged`/`launcherAppsChanged`/`coreModulesChanged` signal flow. Qt's reverse-order child destruction tears PackageCoordinator down first (stops emitting), then UIPluginManager (tears down widgets while the C API handle is still valid), then CoreModuleManager.

## Key QML Files

| File | Purpose |
|------|---------|
| `src/qml/views/OverlayDialogs.qml` | Global dialog layer (missing deps, cascade confirm, install confirm) — hosted in a transparent top-level QQuickWidget |
| `src/qml/controls/ConfirmationDialog.qml` | Multi-mode dialog: `missingDeps`, `unloadCascade`, `uninstallCascade`, `installConfirm` |
| `src/qml/panels/SidebarPanel.qml` | App icons + system nav buttons |
| `src/qml/panels/UiModulesTab.qml` | UI Modules tab in the Modules view |
| `src/qml/views/CoreModulesView.qml` | Core Modules tab with load/unload/uninstall/stats |
| `src/qml/views/ContentViews.qml` | StackLayout switching between Dashboard, Modules, Settings |
| `src/qml/panels/ModuleRow.qml` | Reusable row component for module lists |

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
