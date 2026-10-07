# Logos Basecamp

A Qt/QML desktop application with a plugin-based architecture. It uses Nix for builds and has an MCP-based QML inspector for UI automation.

## Building & Running

```bash
# Build the app
nix build

# Build + run directly
nix build && ./result/bin/LogosBasecamp

# Iterate on the shell's QML without rebuilding — saves are hot reloaded.
nix run .#ui-dev-portable # = DEV_QML_PATH=$PWD/src on the portable build; run from the repo root
nix run .#ui-dev          # the same on the dev build (.#app)
```

QML lives in feature-axis qt_add_qml_module modules (Basecamp.Sidebar,
.AppManager, .Settings, .Shell, plus .Backend for C++ types) — bytecode is
embedded in the main_ui plugin. No runtime QML disk cache, so the qrc-keyed cache
staleness bug doesn't apply.

### `DEV_QML_PATH` — hot reload the shell's QML

`nix run .#ui-dev-portable` / `.#ui-dev` (flake.nix `mkUiDev`, around
`bin-bundle-dir-inspector` / `.#app`) set `DEV_QML_PATH` to `$PWD/src`; set by
hand it works with any build. The catalogue ships only portable variants, so on
the dev build the package manager can install nothing from it. Never default it in the app:
shell QML runs unsandboxed. `src/ShellDevQml.{h,cpp}`:

- A URL interceptor on each shell engine (sidebar, content, overlay, welcome)
  serves every `Basecamp.*` QML file from the source tree — entry files, sibling
  files and module components alike — instead of the copy compiled into
  main_ui. The embedded qmldirs stay in use, so `Basecamp.Backend`'s C++ types
  are unchanged.
- On save, every shell view is emptied, every engine's cache cleared, and each
  view reloaded, then its root signals re-wired (`MainContainer::wireSidebar`,
  `WorkspaceArea::wireWelcomePage`, the overlay's `overlayActiveChanged`). Each
  file is loaded as `<file>?reload=<n>`, so nothing cached survives a reload.
  QML state resets; the backend and loaded apps keep running.
- Reload only on a real change (mtime/size): the watcher also fires on reads.
- A save that does not compile is logged; the next good save brings it back.
- Singletons (`BasecampIcons`, `AppColors`, `DownloadFormat`) need a restart:
  an engine keeps their instances, and `QQmlEngine::clearSingletons()` also
  drops Qt's own `Qt` object, breaking the design system's theme.
- A new QML file must still be listed in `src/CMakeLists.txt` and built once.
- `LOGOS_QML_HOT_RELOAD=0` loads from the source tree without watching.

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
- Session logging (stdout/stderr capture into `logs/`) is configured by an optional `config.yaml` in the session root — see the README's "Session logging" section; the code is `app/utils/LogSink.*` + `app/utils/LoggingConfig.*`
- Main UI is in `src/Basecamp/` (the main_ui plugin), organised by feature: `Sidebar/`, `AppManager/`, `Settings/`, `Shell/`, `Icons/`

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

### MainUIBackend (`app/MainUIBackend.h/.cpp`)
Thin QML-facing facade. Holds only navigation state (`m_currentActiveSectionIndex`, `m_sections`). Every QML-visible slot/signal is a one-line delegation into one of the three managers. The `coreModules()` Q_PROPERTY is the one exception — it composes data from multiple managers (known list + stats from CoreModuleManager, installType from PackageCoordinator). The `cancelPendingAction(name)` slot fans out to both UIPluginManager and PackageCoordinator so the un-involved one no-ops.

### CoreModuleManager (`app/CoreModuleManager.h/.cpp`)
Single owner of the `logos_core_*` C API. Provides thin wrappers: `knownModules()`, `loadedModules()`, `loadModule()`, `unloadModule()`, `unloadModuleWithDependents()`, plus a stats timer that periodically queries `logos_core_get_module_stats`. Nothing else in the app calls the C API directly.

### UIPluginManager (`app/UIPluginManager.h/.cpp`)
Owns UI plugin widget lifecycle in-process: PluginLoader wiring, widget teardown, app launcher state, UI-plugin metadata cache (`m_uiPluginMetadata`) used for load dispatch. Runs the local *unload* cascade (no package_manager involvement). Queries PackageCoordinator for installType / missing-deps / dependents via accessor methods. Exposes `intersectWithLoaded(names)` + `teardownUiPluginWidget(name)` for PackageCoordinator to call during uninstall cascade.

- **Load/unload**: `loadUiModule`, `unloadUiModule`, `loadCoreModule`, `unloadCoreModule` — pre-flight dependency checks, then delegates to CoreModuleManager
- **Unload cascade**: `confirmUnloadCascade`, `cancelUnloadCascade` — single-slot `m_pendingUnload` drives the QML dialog
- **App launcher**: `activateApp`, `onAppLauncherClicked`, `setCurrentVisibleApp`

### PackageCoordinator (`app/PackageCoordinator.h/.cpp`)
Owns every interaction with the `package_manager` LogosAPI module. (Named `PackageCoordinator` rather than `PackageManager` to avoid colliding with the SDK-generated `PackageManager` proxy class.) Event subscriptions, install/uninstall/upgrade IPC, the confirmation dialogs, plus the package-state caches (`m_installTypeByModule`, `m_missingDepsByModule`, `m_blockingDepsByModule` — the same set as the previous one with the reason each entry blocks, `m_dependentsByModule`). Holds the cascade pending slot.

- **Confirmation intents**: `package_manager_ui` raises `basecamp.packages.confirm_install` / `confirm_uninstall` / `confirm_upgrade`; `beginPackageConfirmation()` draws the dialog and takes ownership of answering. The answer IS the permission — PMU removes nothing until it arrives, so the cascade-unload has always finished first. Basecamp initiates no install of its own here.
- **Bound consent**: the dispatch id lives on the thing it is answering (`PendingAction::intentRequestId`, or `m_pendingInstallRequestId` for the cascade-less install), never in a single "whatever is pending" slot. A click on one dialog must not be able to answer another request — the shell's own uninstall dialogs carry no id at all.
- **Undelivered answers**: `finishIntent()` reports whether the broker accepted. `false` means the requester is gone or the dispatch already ended — including when the cascade just tore down PMU itself — so the removal falls back to `performLocalRemoval()` rather than being silently dropped.
- **Cascade confirmation**: `confirmUninstallCascade`, `cancelPendingAction` — drives cascade unload via CoreModuleManager + UIPluginManager, then answers.
- **Metadata refresh**: `refresh()` triggers the full `getInstalledUiPlugins` + `getInstalledPackages` + per-package `resolveFlatDependencies/Dependents` chain; pushes UI metadata to UIPluginManager via `uiPluginsFetched` signal

### App-to-app intents (`app/IntentRegistry`, `app/IntentBroker`)
An app asks for a *capability* (`logos.request("wallet.send", …)`) and the shell picks who services it. Basecamp owns the **disposable** half — resolution, consent, dispatch; the frozen QML surface lives in `logos-view-module-runtime` (`LogosIntent.h`). No policy belongs in the frozen half, so this can be replaced wholesale when the core runtime takes over provider selection.

- **IntentRegistry** — reads `provides` / `uses` / `provides[].params` out of each installed app's `metadata.json`. `ui_qml` only, and that is a design line, not a V1 shortcut: core modules call each other directly through `LogosAPI` with no chooser and nothing to consent to. `logos.*` (the platform's) and `basecamp.*` (this shell's) are both reserved and refused from any on-disk record — the first by the frozen surface, the second by `IntentRegistry`, because which shell owns a prefix is policy.
- **IntentBroker** — the whole lifecycle. Reaches the world through four seams (`IntentEndpoint`, `IntentPresenter`, `IntentChooser`, `IntentInstaller`), all faked in `tests/intent_broker_test.cpp`, so the policy layer is testable with no UI.
- **IntentBridgeAdapter / ShellIntent\*** — bind each UI plugin's `LogosQmlBridge` to the broker, and re-emit chooser/install prompts as QML signals.

Load-bearing invariants, all covered by tests:

- **The requester's `requestId` never leaves its side.** The broker mints a separate `dispatchId`. A response is accepted only if the id is pending, the phase is `Dispatched`, **and** the responding endpoint is pointer-identical to the recorded provider — pointer, not name, so a reloaded app cannot inherit in-flight requests. A failed guard drops silently.
- **`unavailable` merges "nothing installed" with "denied"**, floored at 400 ms, so an app cannot enumerate what you have installed. The install suggestion answers the requester immediately and never completes its request, for the same reason.
- **One dialog at a time.** A second request queues rather than repointing a chooser under the user's cursor — that would be a consent swap.
- **Only a provider's own answer moves the user.** Answering returns them to the requester; the other five `finish()` paths (deadlines, endpoint death, abandon, refusals) never navigate, because nothing on screen would explain it. `"handoff": true` on a `provides` entry opts out entirely — the request existed to take the user somewhere and leave them there. It governs navigation only; when the provider answers (on arrival, or when the user marks the action done) is independent.

### Deep links (`app/links/`)
A `basecamp://` URL from a browser becomes an intent. `SingleInstanceGuard` (socket keyed on the **resolved** user dir, so `--user-dir` still isolates; `--new-instance` skips `acquire()` entirely, and the secondary path prints why it exited rather than returning 0 in silence — README, "Parallel Instances"), `LinkUrlInbox` (where argv, the macOS `QFileOpenEvent` and the socket all land), `LinkUrl` (the parser — pure, and the only fully attacker-controlled surface), `LinkRequestCoordinator` (parks until the first registry rebuild, caps one in flight), `SchemeRegistrar` (Linux `.desktop` + Windows `HKCU`; macOS is the plist).

Two invariants, both tested: a link submits under **its own** requester name, never `main_ui`, or the broker would skip the chooser and every web link would dispatch with no consent; and an app's capability is unreachable from a URL until its author sets `"web": true` on the `provides` entry.

Where things are: dialogs in `src/Basecamp/Shell/Intent*Dialog.qml`, wiring in `Shell/OverlayDialogs.qml`, fixtures in `tests/fixtures/intents/`. Full design and known limitations: `docs/app-to-app-intents.md`.

### Host profile and standalone mode (`app/utils/HostProfile.h`, `app/utils/StandaloneMode.h`)
`main.cpp` builds one `HostProfile` and passes it down `Window` → `MainUIBackend` → `UIPluginManager` / `PackageCoordinator`. Components read capabilities, never "is this standalone". `StandaloneMode` only builds the dev-host profile from `--module` (install roots only — a single module dir is refused, it would expose its siblings) `--modules-dir` and `--qml-source` (into `qmlSources`).
- `main.cpp`: scan roots, user modules dir, `package_downloader`, URL scheme, single instance, default user dir and application name — all from the profile.
- `PackageCoordinator`: profile roots via `addEmbedded*Directory`; user dirs only if `useUserModules`. The downloader subscription depends only on whether the downloader is loaded.
- `UIPluginManager`: `openStartupApps()` opens `appsToOpen` once dependency data is ready; `launcherApps()` filters when `launcherLimitedToOpenedApps`; an app in `qmlSources` gets its `qmlViewPath` from the source tree and a `hotReloadDir`.
- `PluginLoader` / `QmlHotReloadView` (`app/QmlHotReload.h`): with `hotReloadDir`, the app's tab is a `QmlHotReloadView` that owns the `QQuickWidget` and replaces it on save. The old view and engine are deleted before the new one is built — a surviving engine keeps `.pragma library` JS and `.mjs` stale. The bridge moves to the new engine; `WorkspaceArea` finds the view as the container's direct `QQuickWidget` child.
- The shell reads `backend.availableSections` (Sidebar, `MainContainer`'s redirect), `repositoryManagement` (Settings), `packageCatalog` (WelcomePage) and `devHost` (sidebar label). A backend without them (shell-preview) gets the full shell.
- `flake.nix`: `app-standalone` (dev) and `app-standalone-portable` (portable, inspector on) bundle only what `--module` loads; `lib.<system>.mkPluginTest`; checks `standalone-test`, `standalone-backend-test`, `standalone-rejects-module-dir`.

### Construction & Destruction Order
CoreModuleManager is constructed first, UIPluginManager second (receives CoreModuleManager), PackageCoordinator third (receives both). UIPluginManager's `setPackageCoordinator` is called after all three exist, closing the cycle and wiring the `uiPluginsFetched`/`uiModulesChanged`/`launcherAppsChanged`/`coreModulesChanged` signal flow. Qt's reverse-order child destruction tears PackageCoordinator down first (stops emitting), then UIPluginManager (tears down widgets while the C API handle is still valid), then CoreModuleManager.

## Key QML Files

| File | Purpose |
|------|---------|
| `src/Basecamp/Shell/OverlayDialogs.qml` | Global dialog layer (missing deps, cascade confirm, install gate, install failure) — hosted in a transparent top-level QQuickWidget |
| `src/Basecamp/Shell/ConfirmationDialog.qml` | Multi-mode dialog: `missingDeps`, `unloadCascade`, `upgradeCascade`, `installGate`, `installError` — uninstall confirmation lives in `UninstallDialog.qml` |
| `src/Basecamp/Sidebar/SidebarPanel.qml` | App icons + system nav buttons |
| `src/Basecamp/Settings/AppsInspectorView.qml` | Apps Inspector (UI plugins) — view-only, load/unload; uninstall lives in PMUI |
| `src/Basecamp/Settings/ModuleInspectorView.qml` | Module Inspector (core modules) — view-only, load/unload + stats; uninstall lives in PMUI |
| `src/Basecamp/Shell/ContentViews.qml` | StackLayout switching between Dashboard, Repositories, Apps/Module Inspector |

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

- `src/Basecamp/` - QML UI source files, organised by feature (Sidebar/AppManager/Settings/Shell/Icons)
- `nix/` - Nix build configurations (app.nix, smoke-test.nix, integration-test.nix)
- `logos-qt-mcp` - QML Inspector: MCP server, test framework, Qt plugin (separate repo, flake input)
- `tests/` - UI integration tests
- `qt-ios/` - iOS build scripts
