# Logos Basecamp

## Overall Description

Logos Basecamp is a desktop application shell for the Logos modular platform. It provides a unified graphical environment that manages two types of components: **Logos Modules** (Logos Modules managed by `liblogos_core`) and **UI Apps** (Qt plugins loaded directly by Basecamp that provide graphical interfaces). Together, these components deliver functionality like messaging, storage, wallets, and package management through a common navigation model.

The application is designed to:
- Serve as a frontend for the Logos runtime (`liblogos_core`), managing Logos Module lifecycle via its C API
- Host UI Apps as tabbed workspaces in a sidebar-and-content layout, with each app able to call Logos Modules for backend services
- Provide a built-in package manager for installing, browsing, and removing both Logos Modules and UI Apps
- Sandbox untrusted QML UI App content — restricting its network access, filesystem reach, and ability to load native code into the host process
- Offer cross-platform distribution as a self-contained portable application

## Definitions & Acronyms

| Term | Definition |
|------|------------|
| **Logos Module** | An independently developed plugin that implements `PluginInterface` and is managed by the Logos runtime (`liblogos_core`). Each module runs in its own isolated `logos_host` subprocess, communicates via the Logos API, and is authenticated with tokens. Examples: `package_manager`, `capability_module`, `waku_module`. |
| **UI App** | A Qt plugin (C++ shared library or QML package) loaded directly by Basecamp into its own process. UI Apps provide a graphical widget displayed as a tab in the MDI workspace. They are managed entirely by Basecamp, not by liblogos. UI Apps may depend on Logos Modules for backend services. Examples: `package_manager_ui`. |
| **MDI** | Multi-Document Interface — the tabbed content area where UI App windows are displayed |
| **Sidebar** | The left-hand navigation panel listing available UI Apps, system views, and loaded modules |
| **Section** | A named navigation entry in the sidebar; sections are typed as either "workspace" (apps) or "view" (system screens) |
| **LGX** | Logos Package Format — the archive format used to distribute both Logos Modules and UI Apps |
| **Module Directory** | A filesystem path scanned by liblogos for Logos Module shared libraries at startup |
| **Plugin Directory** | A filesystem path scanned by Basecamp for UI App shared libraries or QML packages |
| **Embedded** | Logos Modules or UI Apps bundled into the application at build time (read-only) |
| **User-installed** | Logos Modules or UI Apps installed at runtime to a user-writable directory |
| **Portable Build** | A self-contained build with no external dependency references, suitable for distribution |
| **Dev Build** | A development build that references the Nix store for dependencies |

## Domain Model

### Two Types of Components

Basecamp manages two fundamentally different types of components:

**Logos Modules** are process-isolated modules managed by the Logos runtime (`liblogos_core`). Each Logos Module runs in its own `logos_host` subprocess, communicates with other modules via the Logos API through the remote object registry, and is authenticated with UUID tokens. Basecamp does not load or host these modules itself — it delegates to `liblogos_core` via its C API. Examples include `package_manager` (manages installed packages), `capability_module` (handles authorization tokens), and `waku_module` (peer-to-peer networking). These modules have no UI of their own and run headlessly in the background providing services.

**UI Apps** are Qt plugins (either C++ shared libraries implementing `IComponent`, or QML packages) loaded directly by Basecamp into its own process using Qt's `QPluginLoader` or `QQuickWidget`. UI Apps are not managed by liblogos — Basecamp handles their full lifecycle: discovery, loading, widget creation, tab management, and unloading. UI Apps provide a graphical interface displayed as a tab in the MDI workspace. They typically depend on one or more Logos Modules for backend functionality — for example, `package_manager_ui` calls methods on the `package_manager` Logos Module via LogosAPI.

```
┌─────────────────────────────────────────────────────────────┐
│  Basecamp Process                                           │
│  ┌────────────────────────────────────┐                     │
│  │ Application Shell + Main UI        │                     │
│  │  ├─ Sidebar, MDI, system views     │                     │
│  │  └─ UI App: package_manager_ui ◄───┼── QPluginLoader     │
│  │                                    │   (in-process)       │
│  └────────────────────────────────────┘                     │
│  ┌────────────────────────────────────┐                     │
│  │ liblogos_core (linked library)     │                     │
│  │  ├─ Remote Object Registry         │                     │
│  │  └─ Module lifecycle C API         │                     │
│  └────────────┬───────────────────────┘                     │
│               │ IPC (local sockets)                         │
│         ┌─────┼─────────┬─────────┐                         │
│         ▼     ▼         ▼         ▼                         │
│  ┌──────────┐┌──────────┐┌──────────┐┌──────────┐          │
│  │logos_host││logos_host││logos_host││logos_host│           │
│  │pkg_mgr  ││capability││ waku    ││ storage │           │
│  └──────────┘└──────────┘└──────────┘└──────────┘          │
│  Logos Modules (separate processes, managed by liblogos)     │
└─────────────────────────────────────────────────────────────┘
```

### Application Architecture

At a high level, Basecamp consists of:

**Application Shell** — The main window that initializes the Logos runtime (via `liblogos_core`), creates the navigation layout, and manages the application lifecycle (system tray, window state, platform-specific styling).

**Main UI Plugin** — A dynamically loaded Qt plugin that provides the full user interface: sidebar navigation, content area, module management views, and the MDI workspace. Separating the UI into a plugin allows the shell to remain minimal.

**Backend** — The logic layer (`MainUIBackend`) that coordinates both Logos Module and UI App state — it calls `liblogos_core` to manage Logos Modules, uses `QPluginLoader` to manage UI Apps, collects resource statistics, manages navigation sections, and communicates with the package manager for install/uninstall events.

**MDI View** — A tabbed workspace where each loaded UI App gets its own tab. Tabs can be opened, closed, and switched via the sidebar or the tab bar.

**QML Bridge** — A bridge that exposes Logos Module method calls to QML-based UI Apps, serializing results to JSON for consumption by the declarative UI layer. This is how UI Apps communicate with the backend Logos Modules they depend on.

### Navigation Model

The sidebar organizes content into typed sections:

```
┌──────────────────────────────────────────────────────┐
│  Sidebar                    Content Area             │
│  ┌────────┐                ┌───────────────────────┐ │
│  │ Logo   │                │                       │ │
│  ├────────┤                │  MDI Workspace        │ │
│  │ App 1  │◄──workspace──► │  ┌─────┬─────┬─────┐ │ │
│  │ App 2  │                │  │Tab 1│Tab 2│Tab 3│ │ │
│  │ App 3  │                │  ├─────┴─────┴─────┤ │ │
│  │        │                │  │                  │ │ │
│  │        │                │  │  Active Plugin   │ │ │
│  │        │                │  │  Widget          │ │ │
│  ├────────┤                │  │                  │ │ │
│  │  ⌂     │◄──view───────► │  └──────────────────┘ │ │
│  │  ▣     │  Dashboard     │                       │ │
│  │  ⚙     │  Modules       │  OR                   │ │
│  └────────┘  Settings      │  Dashboard / Modules  │ │
│                            │  / Settings View      │ │
│                            └───────────────────────┘ │
└──────────────────────────────────────────────────────┘
```

- **Workspace sections** appear in the upper sidebar and correspond to launchable apps. Clicking one opens (or focuses) the module in the MDI area.
- **View sections** appear at the bottom of the sidebar and switch the content area to system screens (Dashboard, Modules, Settings).

### Discovery

Basecamp discovers components from two separate sets of directories, reflecting the two component types:

**Logos Module directories** (managed by liblogos):
1. **Embedded modules directory** — Read-only, bundled at build time, relative to the application binary.
2. **User modules directory** — Writable, for runtime-installed modules. Platform-specific path under the user's application data directory.

At startup, `liblogos_core` scans these directories, extracts metadata from each plugin file, and populates the known-modules list. Logos Modules are not loaded until explicitly requested or declared as auto-load (e.g., `package_manager`).

**UI App directories** (managed by Basecamp):
1. **Embedded plugins directory** — Read-only, bundled at build time.
2. **User plugins directory** — Writable, for runtime-installed UI Apps.

Basecamp queries the `package_manager` Logos Module to discover installed UI Apps and their metadata (name, icon, QML or C++ type, dependencies).

### Logos Module Lifecycle

```
Discovered ──► Loading ──► Running ──► Unloading ──► Discovered
   │              │            │            │
   │         Spawn         Active as     Terminate
   │         logos_host    background    host process
   │         process       service       + cleanup
   │         + auth token  (Logos API)
   │
   └── Metadata extracted, shown in Core Modules tab
```

1. **Discovery**: liblogos scans module directories and extracts metadata. The module appears in the Core Modules tab as available but not loaded.
2. **Loading**: liblogos spawns a dedicated `logos_host` process for the module, sends an authentication token via local socket, and waits for the module to register with the remote object registry.
3. **Running**: The module provides services via the Logos API. Other modules and UI Apps can call its methods through LogosAPI. Resource usage (CPU, memory) is monitored by the runtime.
4. **Unloading**: The host process is terminated, associated tokens are cleaned up, and the module returns to the discovered state.

### UI App Lifecycle

```
Discovered ──► Loading ──► Running ──► Unloading ──► Discovered
   │              │            │            │
   │         Load plugin   Displayed    Destroy widget
   │         in-process    as MDI tab   + unload plugin
   │         + create      (user        + remove tab
   │           widget      interacts)
   │
   └── Metadata from package_manager, shown in UI Modules tab
```

1. **Discovery**: Basecamp queries the package manager for installed UI Apps. They appear in the UI Modules tab.
2. **Loading**: Basecamp loads the plugin directly into its own process. For C++ plugins, `QPluginLoader` loads the shared library and calls `createWidget(LogosAPI*)`. For QML plugins, a `QQuickWidget` is created with a sandboxed QML engine (network-deny, filesystem-restricted, and barred from loading native code from the app's own directory — see [QML App Sandboxing](#qml-app-sandboxing)). If the UI App declares Logos Module dependencies, those are loaded first via `logos_core_load_module_with_dependencies()`. A new tab is added to the MDI workspace.
3. **Running**: The UI App's widget is displayed in a tab. The user interacts with it. The app may call Logos Modules via the QML bridge or LogosAPI.
4. **Unloading**: The tab is removed from the MDI area, the widget is destroyed, and the plugin is unloaded. Any Logos Module dependencies remain loaded (they may be shared with other apps).

### Package Management Integration

Basecamp integrates with the package management system for installing both Logos Modules and UI Apps. An LGX package may contain either type — the package manager determines the correct installation directory based on the package contents:

1. User clicks "Install LGX Package" in the Modules view, or uses the Package Manager UI App to browse the online catalog
2. The `package_manager` Logos Module extracts the appropriate platform variant from the LGX archive
3. Logos Module files are placed in the user modules directory; UI App files are placed in the user plugins directory
4. The package manager emits an event (`corePluginFileInstalled` or `uiPluginFileInstalled`)
5. Basecamp listens for these events and automatically refreshes the appropriate list — calling `logos_core_refresh_modules()` for Logos Modules, or re-querying the package manager for UI App metadata
6. The user can then load the newly installed component from the Modules view

## Features & Requirements

### Component Management

#### UI App Management

- List all discovered UI Apps with name, icon, and load status
- Load a UI App by name, automatically loading any Logos Module dependencies first, and creating a tab in the MDI workspace
- Unload a UI App, closing its tab and destroying its widget (Logos Module dependencies remain loaded)
- Display app status in the sidebar

#### Logos Module Management

- List all discovered Logos Modules with name, load status, and resource usage
- Load a Logos Module by name (including its dependency tree), spawning an isolated `logos_host` process
- Unload a Logos Module, terminating its host process
- Display per-module CPU percentage and memory usage, updated every 2 seconds

#### Package Installation

- Install Logos Modules and UI Apps from local LGX package files via the Modules view
- Browse and download packages from the online catalog via the Package Manager UI App
- Auto-discover newly installed components without restarting the application

### Navigation

#### Sidebar

- Display application logo and workspace app icons in the upper section
- Display loaded UI App icons in the middle section with visual indicators for active state
- Display system view icons (Dashboard, Modules, Settings) in the lower section
- Clicking a workspace icon opens or focuses the corresponding UI App tab
- Clicking a loaded app icon activates its tab
- Right-click or close gesture on a loaded app unloads it and removes it from the workspace

#### Content Area

- Stack-based layout switching between system views and the MDI workspace
- MDI area with tab bar for managing multiple open UI App windows
- Dashboard view for overview information
- Modules view with tabs for UI Apps and Logos Modules
- Settings view for application configuration

### QML App Sandboxing

QML-based UI Apps (`ui_qml` modules) load directly into the Basecamp process, so an untrusted app is confined by a sandbox applied to its QML engine (`QmlSandbox::configure`). A `ui_qml` app is QML/JS only and must stay inside its own install directory; the sandbox enforces this on three fronts:

- **Network:** a deny-all network access manager blocks all outgoing HTTP/HTTPS requests from sandboxed content; URL interception prevents navigation to external resources. UI Apps that need network access do so indirectly through Logos Modules (which run in their own process and are not sandboxed by Basecamp).
- **Filesystem:** a URL interceptor resolves only `qrc:` URLs and local files under an allow-list of roots — the app's own dir, the vetted shared Logos QML modules, and Qt's own module dirs. Access to any other path is blocked.
- **Native code:** the app's (untrusted) install dir is kept off the engine's native-plugin search path, and a `qmldir` under the app's own dir may not declare a native `plugin`. This closes a sandbox escape (F-008) in which a `ui_qml` app shipped a `qmldir` `plugin` directive plus a Qt plugin `.so` and had Qt `dlopen()` it into the host process — gaining native code execution that bypasses the network and filesystem restrictions. Qt loads native plugins without consulting the URL interceptor, so the interceptor instead rejects the *qmldir that declares the plugin*; vetted roots (Qt's own modules, which legitimately ship native plugins) are exempt. The `sandbox-test` Nix check guards this regression.

### Desktop Integration

- System tray icon with minimize-to-tray and restore functionality
- Platform-specific window styling (macOS native titlebar integration)
- Application icon and branding

### Distribution

- Portable self-contained builds with no external dependency references
- Linux distribution via AppImage (single executable)
- macOS distribution via DMG with signed .app bundle
- Embedded modules bundled at build time for out-of-box functionality

### QML Inspector

- Development tool for inspecting the running QML object tree
- TCP-based server for remote inspection
- Enabled by default in debug builds, disabled in release builds
- Provides tools for UI testing and automation

## User Journeys & Workflows

### Application Startup

When the user launches Basecamp, the following happens from their perspective:

1. The application window appears with a sidebar on the left and a content area on the right
2. Built-in modules (package manager, capability module) are automatically loaded in the background
3. The sidebar populates with available app icons and system view buttons (Dashboard, Modules, Settings)
4. The content area shows the default view (Dashboard or last-active view)
5. The user can immediately begin loading modules, installing packages, or launching apps

### Browsing and Loading Components

1. User clicks the **Modules** icon in the sidebar bottom section
2. The Modules view opens with two tabs: **UI Apps** and **Logos Modules**
3. The **UI Apps** tab lists all discovered UI Apps with name, icon, and load status
4. The **Logos Modules** tab lists all discovered Logos Modules with name, load status, and resource usage (CPU/memory updated every 2 seconds for loaded modules)
5. User clicks **Load** next to a component name
6. For UI Apps: any required Logos Module dependencies are loaded first, then the app's widget appears as a new tab in the MDI workspace
7. For Logos Modules: a `logos_host` process is spawned and the module starts providing services in the background
8. User clicks **Unload** to stop a component

### Installing a Package

1. User navigates to the Modules view
2. User clicks **Install LGX Package**
3. A file picker dialog opens; user selects a `.lgx` file from their filesystem
4. The package manager extracts the appropriate platform variant and installs files to the correct directory (modules dir for Logos Modules, plugins dir for UI Apps)
5. The component list automatically refreshes to show the newly installed component
6. User can now load the component as described above

### Installing from the Online Catalog

1. User loads the **Package Manager UI** app (or it is already loaded)
2. The Package Manager UI appears as a tab in the MDI workspace
3. User browses or searches the online catalog of available packages
4. User selects a package and clicks download/install
5. The package downloader fetches the `.lgx` file; the package manager installs it
6. The Modules view refreshes to include the new component

### Using a UI App

1. User clicks an app icon in the sidebar's workspace section (top area)
2. If the app is not yet loaded, it is loaded automatically — any required Logos Module dependencies are loaded first via liblogos
3. The app's UI appears as a new tab in the MDI workspace
4. User interacts with the app through its own interface (e.g., chat, file storage, wallet)
5. The app may call Logos Modules in the background via LogosAPI (e.g., a chat UI App calls the `waku_module` Logos Module for networking)
6. User can switch between multiple loaded apps via the sidebar icons or the MDI tab bar
7. Closing a tab (via the tab bar close button or sidebar close gesture) unloads the UI App but leaves its Logos Module dependencies running

### Monitoring Logos Module Resources

1. User navigates to the Modules view → Logos Modules tab
2. For each loaded Logos Module, CPU percentage and memory usage are displayed
3. Statistics are polled from `liblogos_core` every 2 seconds
4. If a module is consuming excessive resources, the user can unload it

### Minimizing to System Tray

1. User minimizes or closes the application window
2. The application continues running in the system tray
3. All loaded Logos Modules remain active in their host processes in the background
4. Clicking the tray icon restores the window with all UI App tabs and state intact

## Inter-Component Communication

UI Apps communicate with Logos Modules through the Logos API layer. Since UI Apps run in the Basecamp process and Logos Modules run in isolated `logos_host` processes, all communication crosses a process boundary via the Logos API:

```
UI App (QML)
    │
    ▼
LogosQmlBridge.callModule(module, method, args)
    │
    ▼
LogosAPI → Remote Object Registry → Logos Module host process
    │
    ▼
JSON result returned to QML
```

- QML-based UI Apps use the QML bridge (`logos.callModule(...)`) to call any loaded Logos Module
- C++ UI Apps use `LogosAPI` directly for inter-module calls
- All Logos API calls are authenticated via the token-based system provided by `liblogos_core`
- Results are serialized to JSON for QML consumption
- Logos Modules can also call each other via the same Logos API mechanism, independent of Basecamp

## Supported Platforms

- Linux (x86_64, aarch64)
- macOS (x86_64, aarch64)

## Future Work

- **Module updates** — Automatic detection and installation of module updates
- **Workspace persistence** — Save and restore the set of loaded modules and tab layout across sessions
- **Theme customization** — User-selectable color themes beyond the default design system
- **iOS support** — Full iOS build and distribution pipeline (currently experimental)
