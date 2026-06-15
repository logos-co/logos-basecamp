# Project Description

## Project Structure

```
logos-basecamp/
├── CMakeLists.txt                        # Root CMake configuration
├── README.md                             # Project overview and build instructions
├── CLAUDE.md                             # Developer notes
├── metadata.json                         # Package metadata
├── flake.nix                             # Nix flake configuration
├── flake.lock                            # Nix flake lock file
├── docs/
│   ├── index.md                          # Documentation index
│   ├── spec.md                           # High-level specification
│   └── project.md                        # This document
├── app/                                  # Main application executable
│   ├── CMakeLists.txt                    # App build configuration
│   ├── main.cpp                          # Entry point
│   ├── window.h/cpp                      # Main window (QMainWindow)
│   ├── interfaces/                       # Component interfaces (IComponent)
│   ├── utils/                            # Utility classes (paths, file helpers)
│   ├── macos/                            # macOS-specific code (titlebar styling)
│   └── icons/                            # Application icons
├── src/                                  # Main UI plugin
│   ├── CMakeLists.txt                    # Plugin build configuration
│   ├── main_ui_plugin.h/cpp              # Plugin entry point (IComponent impl)
│   ├── MainContainer.h/cpp               # UI coordinator (sidebar + content)
│   ├── MainUIBackend.h/cpp               # Core logic (module state, stats, navigation)
│   ├── LogosQmlBridge.h/cpp              # QML-to-C++ module call bridge
│   ├── mdiview.h/cpp                     # MDI tab workspace
│   ├── mdichild.h/cpp                    # Individual plugin tab window
│   ├── metadata.json                     # Plugin metadata
│   ├── qml/                              # QML UI files
│   │   ├── panels/
│   │   │   ├── SidebarPanel.qml          # Sidebar navigation
│   │   │   └── UiModulesTab.qml          # Module management tab
│   │   ├── views/
│   │   │   ├── ContentViews.qml          # Content area stack layout
│   │   │   ├── DashboardView.qml         # Dashboard screen
│   │   │   ├── ModulesView.qml           # Modules management screen
│   │   │   ├── CoreModulesView.qml       # Core modules list
│   │   │   ├── PluginInterfaceView.qml   # Interface inspection (methods + events)
│   │   │   └── SettingsView.qml          # Settings screen
│   │   └── controls/                     # Reusable QML controls
│   │       ├── SidebarIconButton.qml
│   │       ├── SidebarAppDelegate.qml
│   │       └── SidebarCircleButton.qml
│   ├── restricted/                       # ui_qml sandbox (network + filesystem + native-plugin)
│   │   ├── QmlSandbox.h/cpp               # applies the sandbox policy to a QML engine
│   │   ├── DenyAllNetworkAccessManager.h/cpp
│   │   ├── DenyAllNAMFactory.h/cpp
│   │   ├── DenyAllReply.h/cpp
│   │   └── RestrictedUrlInterceptor.h/cpp
│   └── icons/                            # Plugin icons and QML icon resources
├── tests/                                # Integration tests
│   └── ui-tests.mjs                      # Node.js test suite (logos-qt-mcp)
├── nix/                                  # Nix build modules
│   ├── default.nix                       # Common build settings
│   ├── app.nix                           # Application package
│   ├── main-ui.nix                       # Main UI plugin build
│   ├── smoke-test.nix                    # Smoke test derivation
│   ├── integration-test.nix              # UI integration test harness
│   ├── appimage.nix                      # Linux AppImage packaging
│   ├── macos-bundle.nix                  # macOS .app bundle
│   └── macos-dmg.nix                     # macOS DMG packaging
├── qt-ios/                               # iOS build configuration (experimental)
│   ├── CMakeLists.txt
│   ├── Main.qml
│   └── metadata.json
├── scripts/                              # Build and utility scripts
├── assets/                               # Application assets
└── LICENSE-*                             # MIT and Apache 2.0 licenses
```

## Stack, Frameworks & Dependencies

| Component | Purpose |
|-----------|---------|
| **C++17** | Implementation language |
| **CMake 3.16+** | Build system |
| **Qt 6** (Widgets, RemoteObjects, Quick, Qml, QuickWidgets, QuickControls2, WebView) | GUI framework, plugin system, IPC, declarative UI |
| **QML** | Declarative UI layer for sidebar, views, and module controls |
| **Nix** | Package management and reproducible builds |
| **Ninja** | Build backend (via Nix) |

### External Logos Dependencies

| Dependency | Purpose |
|------------|---------|
| **[logos-cpp-sdk](https://github.com/logos-co/logos-cpp-sdk)** | SDK root: LogosAPI, code generator, IPC layer. Pins nixpkgs/Qt. |
| **[logos-liblogos](https://github.com/logos-co/logos-liblogos)** | Core library: `liblogos_core` C API, `logos_host` subprocess host |
| **[logos-module](https://github.com/logos-co/logos-module)** | Module introspection library and `lm` CLI |
| **[logos-package-manager-module](https://github.com/logos-co/logos-package-manager-module)** | Package management module (auto-loaded at startup) |
| **[logos-package-downloader-module](https://github.com/logos-co/logos-package-downloader-module)** | Online catalog browsing and package download |
| **[logos-capability-module](https://github.com/logos-co/logos-capability-module)** | Inter-module authorization and token management |
| **[logos-package-manager-ui](https://github.com/logos-co/logos-package-manager-ui)** | Package manager UI module (embedded at build time) |
| **[logos-design-system](https://github.com/logos-co/logos-design-system)** | Centralized color/theme definitions (LogosText, LogosButton) |
| **[logos-qt-mcp](https://github.com/logos-co/logos-qt-mcp)** | QML inspector plugin for testing and development |

### Embedded Components (bundled at build time)

**Logos Modules** (managed by liblogos, run in isolated `logos_host` processes):

| Module | Purpose |
|--------|---------|
| `package_manager` | Package management (auto-loaded at startup) |
| `package_downloader` | Online catalog browsing and download |
| `capability_module` | Inter-module authorization and token management |

**UI Apps** (Qt plugins loaded directly by Basecamp, displayed as MDI tabs):

| App | Type | Purpose |
|-----|------|---------|
| `package_manager_ui` | QML | Package management UI |

## How liblogos Is Used

Basecamp is a frontend for `liblogos_core`, the C shared library that provides the Logos module runtime. The application binary links against `liblogos_core` and uses its C API to manage the lifecycle of **Logos Modules** — the process-isolated backend services. Basecamp does **not** use liblogos for managing **UI Apps** (Qt plugins) — those are loaded and managed by Basecamp itself using Qt's `QPluginLoader` and `QQuickWidget`.

The SDK wrapper (`LogosAPI` from logos-cpp-sdk) is used on top of the C API to provide ergonomic inter-component communication — it is how UI Apps call methods on Logos Modules via the Logos API.

### C API Call Sites

All `logos_core_*` calls are made from two locations: `app/main.cpp` (startup/shutdown) and `src/MainUIBackend.cpp` (runtime module management).

**Initialization and startup** (`app/main.cpp`):

| Call | Purpose |
|------|---------|
| `logos_core_add_modules_dir(embeddedDir)` | Add the embedded modules directory (read-only, pre-installed at build time) |
| `logos_core_add_modules_dir(userDir)` | Add the user-writable modules directory for runtime installs |
| `logos_core_start()` | Scan module directories, initialize the capability module, start the remote object registry |
| `logos_core_load_module("package_manager", true)` | Auto-load the package manager module (with dependencies) at startup |
| `logos_core_get_loaded_modules()` | Query loaded module names for initial status display |

**Runtime Logos Module management** (`src/MainUIBackend.cpp`):

| Call | Purpose |
|------|---------|
| `logos_core_load_module(name, true)` | Load a Logos Module and all its declared dependencies (topological sort). Also called when loading a UI App that depends on Logos Modules. |
| `logos_core_unload_module(name, false)` | Terminate a Logos Module's host process and clean up |
| `logos_core_refresh_modules()` | Re-scan module directories after a package install |
| `logos_core_get_loaded_modules()` | Query which Logos Modules are currently running (for Modules view status) |
| `logos_core_get_known_modules()` | Query all discovered Logos Modules (for Modules view listing) |
| `logos_core_get_module_stats()` | Get JSON-formatted CPU/memory stats for all loaded Logos Modules (polled every 2s) |

**Shutdown** (`app/main.cpp`):

| Call | Purpose |
|------|---------|
| `logos_core_cleanup()` | Terminate all module host processes and release resources |

### LogosAPI Usage

A single `LogosAPI` instance is created in `main()` with the module name `"core"` and passed through the component hierarchy: `main() → Window → main_ui plugin → MainContainer → MainUIBackend`.

**Getting module clients:**
```cpp
LogosAPIClient* client = m_logosAPI->getClient("package_manager");
if (client && client->isConnected()) {
    QVariant result = client->invokeRemoteMethod("package_manager", "methodName", arg1);
}
```

**Using the generated wrapper:**
```cpp
LogosModules logos(m_logosAPI);
logos.package_manager.installPluginAsync(filePath, false, callback);
logos.package_manager.on("corePluginFileInstalled", [](const QVariantList& data) { ... });
```

**QML bridge:** When loading a QML-based UI App, a `LogosQmlBridge` is created and injected into the QML context as `logos`. QML code calls `logos.callModule("module", "method", [args])` which dispatches through `LogosAPIClient::invokeRemoteMethod()` to call Logos Modules via the Logos API, returning a JSON-serialized result.

## Core Modules

### Application Entry Point

**Files:** `app/main.cpp`

**Purpose:** Initializes the Qt application, configures plugin directories (embedded + user-writable), calls `logos_core_start()` to boot the runtime, auto-loads the `package_manager` module, creates the `LogosAPI` instance, creates the main window, starts a stats polling timer (2s interval), starts the QML inspector (if enabled), and runs the event loop. On exit, calls `logos_core_cleanup()`.

### Window

**Files:** `app/window.h`, `app/window.cpp`

**Purpose:** Main `QMainWindow` derivative. Uses `QPluginLoader` to load the `main_ui` plugin from the embedded or user plugins directory (platform-specific extension: `.so`/`.dylib`/`.dll`). Invokes `createWidget(LogosAPI*)` on the plugin to obtain the main content widget. Manages system tray integration (minimize/restore) and applies platform-specific window styling (macOS native titlebar).

### MainUIBackend

**Files:** `src/MainUIBackend.h`, `src/MainUIBackend.cpp`

**Purpose:** Core logic layer exposed to QML. Central coordinator for both Logos Modules and UI Apps — calls `liblogos_core` to manage Logos Modules, uses `QPluginLoader`/`QQuickWidget` to manage UI Apps, polls stats, handles package install events, and manages navigation. This is where most `logos_core_*` calls and `LogosAPI` interactions happen.

**Properties (exposed to QML):**

| Property | Type | Description |
|----------|------|-------------|
| `currentActiveSectionIndex` | int | Currently selected navigation section |
| `sections` | QVariantList | Navigation entries (Dashboard, Modules, Settings, apps) |
| `uiModules` | QVariantList | Discovered UI Apps with load status |
| `coreModules` | QVariantList | Discovered Logos Modules with load status and CPU/memory stats |
| `launcherApps` | QVariantList | UI Apps available for launching |
| `currentVisibleApp` | QString | Currently focused UI App name |

**Key Methods:**

| Method | Description |
|--------|-------------|
| `loadUiModule(name)` | Load a UI App (QML or C++ plugin) — resolve Logos Module dependencies via `logos_core_load_module(name, true)`, then load the Qt plugin and create a tab in MDI |
| `unloadUiModule(name)` | Remove tab from MDI, destroy widget, clean up tracking state. Logos Module dependencies are left running. |
| `loadCoreModule(name)` | Load a Logos Module via `logos_core_load_module(name, true)`, spawning a `logos_host` process |
| `unloadCoreModule(name)` | Unload a Logos Module via `logos_core_unload_module(name, false)`, terminating its host process |
| `refreshCoreModules()` | Call `logos_core_refresh_modules()` then `logos_core_get_known_modules()` to refresh the Logos Module list |
| `updateModuleStats()` | Call `logos_core_get_module_stats()`, parse JSON, update `m_moduleStats` map for Logos Modules |
| `subscribeToPackageInstallationEvents()` | Register event listeners on `package_manager` Logos Module for `corePluginFileInstalled` and `uiPluginFileInstalled` events |
| `fetchUiPluginMetadata()` | Async call to `package_manager.getInstalledUiPluginsAsync()` to populate UI App metadata cache |
| `installPluginFromPath(path)` | Async call to `package_manager.installPluginAsync()` to install an LGX package |

### MainContainer

**Files:** `src/MainContainer.h`, `src/MainContainer.cpp`

**Purpose:** UI coordinator that creates the `MainUIBackend`, assembles the sidebar (QML `SidebarPanel`) and content area (stacked widget with MdiView + QML system views), and routes navigation signals between them.

### LogosQmlBridge

**Files:** `src/LogosQmlBridge.h`, `src/LogosQmlBridge.cpp`

**Purpose:** Bridge between QML-based UI Apps and Logos Modules. Injected into each QML UI App's context as `logos`, enabling UI Apps to call Logos Module methods via the Logos API.

**API:**

| Method | Description |
|--------|-------------|
| `callModule(module, method, args) → QString` | Get a `LogosAPIClient` for the target Logos Module, call `invokeRemoteMethod()`, serialize the result to JSON |

The bridge validates that the `LogosAPI` is available and the target Logos Module is connected before dispatching. Results are serialized to JSON (objects, arrays, primitives) for consumption by QML.

### MdiView

**Files:** `src/mdiview.h`, `src/mdiview.cpp`

**Purpose:** Multi-document interface workspace. Manages a tab bar with one tab per loaded UI App. Handles tab creation, removal, switching, and custom styling with close buttons.

### MdiChild

**Files:** `src/mdichild.h`, `src/mdichild.cpp`

**Purpose:** Individual tab in the MDI area. Wraps a UI App's widget and manages its lifecycle within the tabbed workspace.

### QML Sandbox

**Files:** `src/restricted/QmlSandbox.h/cpp`, `src/restricted/DenyAllNetworkAccessManager.h/cpp`, `src/restricted/DenyAllNAMFactory.h/cpp`, `src/restricted/DenyAllReply.h/cpp`, `src/restricted/RestrictedUrlInterceptor.h/cpp`

**Purpose:** Security layer for QML-based UI Apps (`ui_qml` modules), applied by `QmlSandbox::configure()` (the single setup `PluginLoader::loadQmlView` runs on each app's `QQmlEngine`). A `ui_qml` app is meant to be QML/JS only, confined to its own install directory; the sandbox enforces that on three fronts:

- **Network:** a `DenyAllNAMFactory` blocks all outgoing HTTP/HTTPS. Apps that need network do so indirectly through Logos Modules via the QML bridge.
- **Filesystem:** a `RestrictedUrlInterceptor` resolves only `qrc:` URLs and local files under an allow-list of roots (the app's own dir, the vetted app lib dir's shared Logos QML modules, and Qt's own module dirs). Other schemes and *existing* paths outside the roots are blocked. A non-existent path is passed through untouched — Qt's module resolution probes many non-existent `<importPath>/<Module>[.ver]/qmldir` candidates before finding the real one, and a path that doesn't exist can load nothing; if it later resolves to a real file, that file is re-intercepted (now with a non-empty canonical path) and vetted against the roots then. The only **fail-closed** case is the genuinely anomalous one — a path that *exists* but still won't canonicalise (e.g. a symlink loop) — which is blocked because it can't be vetted yet could back a real resource.
- **Native code:** the app's install dir is **not** added to the engine's native-plugin search path, and a qmldir living under the app's own (untrusted) dir may **not** declare a native `plugin`. Without this, a `ui_qml` app could ship a `qmldir` with a `plugin` directive plus a matching Qt plugin `.so` and have Qt `dlopen()` it straight into the host process — full native code execution, defeating the network/filesystem guarantees (formerly tracked as finding F-008). Native plugin loading bypasses URL interception entirely, so the qmldir that *declares* the plugin is the choke point: rejecting that qmldir makes the malicious module simply "not installed". Vetted roots (the app lib dir, Qt's module dirs — which legitimately ship native plugins like QtQuick) are exempt.

The escape and its fix are covered by the `sandbox-test` check (`tests/sandbox/`, `nix build .#sandbox-test`), which builds a real malicious QML plugin and asserts it is never loaded while a legitimate pure-QML module still is. The same check also regresses the rest of the `ui_qml` sandbox policy — network deny (HTTP and `file://`), URL-interceptor blocking of remote-scheme loads and out-of-root file reads, and the matching positive cases (files under the module's own dir and `qrc:` resources still resolve) — i.e. the guarantees the `counter_qml` probe app exercises by hand, now driven against the real `QmlSandbox::configure`. On top of those mechanism-level slots, `tests/sandbox/evil_app/` is an end-to-end adversarial fixture — the evil twin of `counter_qml` — a real `ui_qml` view whose `Main.qml` automatically fires every escape vector on load and tallies an `escapes` count; the check loads it through the real sandbox and asserts `escapes == 0` (plus a QML-only F-008 probe: an `EvilModule/qmldir` declaring a native plugin must be rejected at import).

| Class | Description |
|-------|-------------|
| `QmlSandbox` (namespace) | `configure(engine, installDir, qmlViewPath, appLibDir)` — applies the whole ui_qml sandbox policy to a QML engine. Factored out of `PluginLoader` so it is unit-testable against a bare `QQmlEngine`. |
| `DenyAllNetworkAccessManager` | Qt network access manager that rejects all requests |
| `DenyAllNAMFactory` | Factory that creates deny-all NAM instances for QML engines |
| `DenyAllReply` | Network reply that immediately signals error |
| `RestrictedUrlInterceptor` | URL interceptor: gates file/qmldir resolution to allowed roots (non-existent probe candidates pass through so Qt's module resolution still works; an *existing* path that can't be canonicalised fails closed), and rejects a qmldir under an *untrusted* root that declares a native plugin |

### Main UI Plugin

**Files:** `src/main_ui_plugin.h`, `src/main_ui_plugin.cpp`

**Purpose:** Plugin entry point implementing `IComponent`. Its `createWidget(LogosAPI*)` method creates a `MainContainer`, which in turn creates the `MainUIBackend` and the full UI layout. Registered via `Q_PLUGIN_METADATA`.

## QML UI Layer

### SidebarPanel

**File:** `src/qml/panels/SidebarPanel.qml`

**Purpose:** Left-hand navigation panel. Sections are filtered by type — "workspace" entries appear at the top, "view" entries at the bottom. Loaded apps appear in the middle with close/activate interactions.

### ContentViews

**File:** `src/qml/views/ContentViews.qml`

**Purpose:** Content area using `StackLayout` with four indices: MDI area (index 0), Dashboard (1), Modules (2), Settings (3). The active index is controlled by the sidebar selection.

### ModulesView

**File:** `src/qml/views/ModulesView.qml`

**Purpose:** Component management screen with two tabs: **UI Apps** (Qt plugins managed by Basecamp) and **Logos Modules** (process-isolated modules managed by liblogos). Lists available/loaded components with load/unload buttons, icons, and status indicators. The Logos Modules tab also shows CPU/memory stats for running modules. Includes "Install LGX Package" action.

### DashboardView / SettingsView

**Files:** `src/qml/views/DashboardView.qml`, `src/qml/views/SettingsView.qml`

**Purpose:** System views for overview information and application configuration.

## Sequence Flows

### Application Startup

```
main()
 ├─ QApplication(argc, argv)
 ├─ logos_core_add_modules_dir(<app>/../modules)       # Embedded modules (read-only)
 ├─ logos_core_add_modules_dir(~/.local/share/.../modules)  # User modules (writable)
 ├─ logos_core_start()                                 # Scan dirs, init capability module, start registry
 ├─ logos_core_load_module("package_manager", true)    # Auto-load package manager
 ├─ logos_core_get_loaded_modules()                    # Log loaded modules
 ├─ LogosAPI("core", nullptr)                          # Create SDK instance
 ├─ Window(&logosAPI)
 │   └─ QPluginLoader loads main_ui plugin
 │       └─ main_ui_plugin::createWidget(&logosAPI)
 │           └─ MainContainer(logosAPI)
 │               ├─ MainUIBackend(logosAPI)
 │               │   ├─ initializeSections()           # Dashboard, Modules, Settings + app sections
 │               │   ├─ m_statsTimer.start(2000)       # Poll module stats every 2s
 │               │   ├─ refreshCoreModules()
 │               │   │   └─ logos_core_refresh_modules()
 │               │   ├─ subscribeToPackageInstallationEvents()
 │               │   │   ├─ logos.package_manager.setUserModulesDirectory(...)
 │               │   │   ├─ logos.package_manager.on("corePluginFileInstalled", ...)
 │               │   │   └─ logos.package_manager.on("uiPluginFileInstalled", ...)
 │               │   └─ fetchUiPluginMetadata()
 │               │       └─ logos.package_manager.getInstalledUiPluginsAsync(callback)
 │               └─ setupUi()                          # Create sidebar + content area + MDI
 ├─ statsTimer.start(2000)                             # Console stats logging
 ├─ QML Inspector start (if enabled)
 ├─ app.exec()                                         # Qt event loop
 └─ logos_core_cleanup()                               # Terminate all modules on exit
```

### Loading a UI App (QML)

```
User clicks "Load" in UI Apps tab (or clicks app icon in sidebar)
 └─ MainUIBackend::loadUiModule(name)
     ├─ Look up app metadata from m_uiPluginMetadata cache
     ├─ Load Logos Module dependencies (if any)
     │   └─ logos_core_load_module(dep, true) for each dependency
     ├─ Create QQuickWidget (loaded in Basecamp process, NOT via liblogos)
     ├─ Configure QML engine:
     │   ├─ Set import/plugin paths
     │   ├─ Install DenyAllNAMFactory (block network)
     │   ├─ Install RestrictedUrlInterceptor (whitelist app dir only)
     │   └─ Set base URL to app directory
     ├─ Create LogosQmlBridge(logosAPI) → inject as "logos" context property
     ├─ Load QML source file
     ├─ Store widget in m_qmlPluginWidgets and m_uiModuleWidgets
     ├─ emit pluginWindowRequested(widget, name) → MdiView adds tab
     └─ emit navigateToApps() → sidebar switches to Apps view
```

### Loading a UI App (C++ Qt Plugin)

```
User clicks "Load" in UI Apps tab
 └─ MainUIBackend::loadUiModule(name)
     ├─ QPluginLoader(pluginPath).load()  (loaded in Basecamp process, NOT via liblogos)
     ├─ qobject_cast<IComponent*>(plugin)
     ├─ component->createWidget(logosAPI) → get QWidget
     ├─ Store in m_loadedUiModules and m_uiModuleWidgets
     ├─ emit pluginWindowRequested(widget, name) → MdiView adds tab
     └─ emit navigateToApps()
```

### Unloading a UI App

```
User clicks close on tab or "Unload" in UI Apps tab
 └─ MainUIBackend::unloadUiModule(name)
     ├─ emit pluginWindowRemoveRequested(widget) → MdiView removes tab
     ├─ For C++ apps: component->destroyWidget(widget)
     ├─ For QML apps: widget->deleteLater()
     ├─ Remove from all tracking maps
     ├─ emit uiModulesChanged(), launcherAppsChanged()
     └─ (Logos Module dependencies remain running)
```

### Loading/Unloading a Logos Module

```
User clicks "Load" in Logos Modules tab
 └─ MainUIBackend::loadCoreModule(name)
     ├─ logos_core_load_module(name, true)              # liblogos spawns logos_host process
     └─ emit coreModulesChanged()

User clicks "Unload" in Logos Modules tab
 └─ MainUIBackend::unloadCoreModule(name)
     ├─ logos_core_unload_module(name, false)  # liblogos terminates logos_host process
     └─ emit coreModulesChanged()
```

### Package Installation

```
User clicks "Install LGX Package" → selects .lgx file
 └─ MainUIBackend::installPluginFromPath(filePath)
     └─ logos.package_manager.installPluginAsync(filePath, false, callback)
         ├─ Package manager extracts platform variant from LGX archive
         ├─ Files copied to user modules/plugins directory
         ├─ Event emitted: "corePluginFileInstalled" or "uiPluginFileInstalled"
         │   └─ MainUIBackend event handler:
         │       ├─ refreshCoreModules()
         │       │   └─ logos_core_refresh_modules()  # Re-scan directories
         │       └─ fetchUiPluginMetadata()           # Refresh UI plugin list
         └─ Async callback:
             ├─ refreshCoreModules()
             └─ fetchUiPluginMetadata()
```

### Stats Polling (Logos Modules only)

```
Every 2 seconds (m_statsTimer):
 └─ MainUIBackend::updateModuleStats()
     ├─ logos_core_get_module_stats() → JSON string (Logos Module processes only)
     ├─ Parse JSON array: [{name, cpu_percent, memory_mb}, ...]
     ├─ Store in m_moduleStats map
     └─ emit coreModulesChanged() → QML updates Logos Modules tab
```

Note: Stats are only available for Logos Modules because they run in separate `logos_host` processes that liblogos can monitor. UI Apps run in the Basecamp process itself and are not separately monitored.

### UI App Calling a Logos Module

```
QML UI App code: logos.callModule("storage_module", "getFiles", ["/data"])
 └─ LogosQmlBridge::callModule("storage_module", "getFiles", ["/data"])
     ├─ m_logosAPI->getClient("storage_module") → LogosAPIClient*
     ├─ Check client != null && client->isConnected()
     ├─ client->invokeRemoteMethod("storage_module", "getFiles", "/data")
     │   └─ LogosAPI → Remote Object Registry → storage_module logos_host process → method call → result
     └─ Serialize QVariant result to JSON string → return to QML
```

## Component Directory Resolution

Logos Modules and UI Apps are discovered from separate directories, reflecting their different management layers.

### Logos Module Directories (managed by liblogos)

Configured via `logos_core_add_modules_dir()` in `main.cpp` (embedded and user-writable directories). liblogos scans these directories for `.so`/`.dylib`/`.dll` files and extracts module metadata.

**Embedded (read-only):**
- `<app-dir>/../modules/`

**User-installed (writable):**
- **macOS:** `~/Library/Application Support/Logos/LogosBasecampDev/modules/`
- **Linux:** `~/.local/share/Logos/LogosBasecampDev/modules/`

### UI App Directories (managed by Basecamp)

Basecamp discovers UI Apps by querying the `package_manager` Logos Module and resolving paths via `LogosBasecampPaths`.

**Embedded (read-only):**
- `<app-dir>/../plugins/`

**User-installed (writable):**
- **macOS:** `~/Library/Application Support/Logos/LogosBasecampDev/plugins/`
- **Linux:** `~/.local/share/Logos/LogosBasecampDev/plugins/`

All directory paths are managed via the `LogosBasecampPaths` utility class.

## Build Artifacts

| Artifact | Description |
|----------|-------------|
| `bin/LogosBasecamp` | Main application executable |
| `lib/liblogos_core.{so,dylib}` | Core library (from logos-liblogos) |
| `bin/logos_host` | Module subprocess host (from logos-liblogos) |
| `modules/` | Embedded Logos Module bundles |
| `plugins/main_ui/` | Main UI plugin (Basecamp's own UI, not a user-facing UI App) |
| `plugins/*/` | Embedded UI App bundles |

### Distribution Artifacts

| Artifact | Platform | Description |
|----------|----------|-------------|
| AppImage | Linux | Single-file self-contained executable |
| .app bundle | macOS | Ad-hoc signed application bundle |
| DMG | macOS | Disk image for distribution |

## Operational

### Nix (Recommended)

Nix provides reproducible builds with all dependencies managed automatically.

**Build the application:**

```bash
nix build
```

The result includes the application binary at `result/bin/LogosBasecamp` with all embedded modules.

**Build individual outputs:**

```bash
nix build '.#app'                  # Standard development build
nix build '.#portable'             # Self-contained portable build
nix build '.#bin-appimage'         # Linux AppImage
nix build '.#bin-macos-app'        # macOS .app bundle
nix build '.#bin-macos-dmg'        # macOS DMG
nix build '.#logos-qt-mcp'         # QML inspector for testing
```

**Run tests:**

```bash
# Smoke test (validates app starts without errors)
nix build '.#smoke-test' -L

# Integration tests
nix build '.#logos-qt-mcp' -o result-mcp
node tests/ui-tests.mjs --ci ./result/bin/LogosBasecamp
```

**Development shell:**

```bash
nix develop
```

**Override local dependencies:**

```bash
nix build --override-input logos-liblogos path:../logos-liblogos
nix build --override-input logos-cpp-sdk path:../logos-cpp-sdk
```

### Workspace CLI

From the logos-workspace root:

```bash
ws build logos-basecamp                    # Build
ws build logos-basecamp --auto-local       # Build with local overrides for dirty deps
ws test logos-basecamp                     # Run tests
ws run logos-basecamp                      # Build and run
ws develop logos-basecamp                  # Enter dev shell
```

### CMake

**Prerequisites:**
- CMake 3.16+
- C++17 compatible compiler
- Qt 6 with Widgets, RemoteObjects, Quick, Qml, QuickWidgets, QuickControls2 modules

**Build:**

```bash
nix develop                     # Get all dependencies
mkdir -p build && cd build
cmake ..
cmake --build . -j$(nproc)
```

**CMake Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `LOGOS_PORTABLE_BUILD` | OFF | Self-contained build for distribution |
| `LOGOS_DISTRIBUTED_BUILD` | OFF | For AppImage/DMG packaging |
| `ENABLE_QML_INSPECTOR` | ON (Debug), OFF (Release) | Enable QML inspector server |

### Dev vs Portable Builds

- **Dev build** (default): Modules loaded with `-dev` variant suffix. Dependencies reference the Nix store.
- **Portable build** (`LOGOS_PORTABLE_BUILD=ON`): Modules loaded without suffix. All dependencies bundled. No Nix store references at runtime.

### Environment Variables

| Variable | Purpose |
|----------|---------|
| `LOGOS_USER_DIR` | Override application base directory as-is (also settable via `--user-dir`) |
| `QML_INSPECTOR_PORT` | QML inspector server port (default: 3768) |

## Testing

### Smoke Test

**File:** `nix/smoke-test.nix`

Validates the application starts correctly:
- Runs with `-platform offscreen` (headless)
- Checks for QML errors, crashes, and `qCritical` output
- 5-second timeout
- Logs saved to `result/smoke-test.log`

```bash
nix build '.#smoke-test' -L
```

### Integration Tests

**File:** `tests/ui-tests.mjs`

End-to-end UI tests using the `logos-qt-mcp` framework:
- Clicks buttons, verifies visible text
- Tests package manager and core modules
- Supports CI mode (headless) and interactive mode
- Skips GPU-dependent tests in offscreen mode

```bash
nix build '.#logos-qt-mcp' -o result-mcp
node tests/ui-tests.mjs --ci ./result/bin/LogosBasecamp
```

### QML Inspector

Development tool for inspecting the running QML tree over TCP:
- Default port: 3768 (localhost)
- Tools: `qml_find_and_click`, `qml_screenshot`, `qml_get_tree`, `qml_list_interactive`
- Used by integration tests and AI agents for UI automation

## Continuous Integration

GitHub Actions workflow (`.github/workflows/ci.yml`) runs on every push/PR to `master`:

1. Checkout code
2. Install Nix with flakes enabled
3. Use cachix cache
4. Build the application
5. Run smoke tests

## Supported Platforms

- Linux (x86_64, aarch64) — AppImage distribution
- macOS (x86_64, aarch64) — DMG distribution

## Known Limitations

1. **No workspace persistence** — The set of loaded modules and tab layout is not saved across application restarts.
2. **No module updates** — There is no mechanism to detect or install module updates automatically.
4. **QML inspector in release** — The inspector is disabled in release builds and cannot be enabled at runtime.
