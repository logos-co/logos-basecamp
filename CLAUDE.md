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
.Settings, .Shell, .Common, .Installer, .Icons, plus .Backend for C++ types) —
bytecode is embedded in the main_ui plugin. No runtime QML disk cache, so the qrc-keyed cache
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
- Singletons (`BasecampIcons`) need a restart: an engine keeps their
  instances, and `QQmlEngine::clearSingletons()` also drops Qt's own `Qt`
  object, breaking the design system's theme.
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
- **Plugins** appear as sidebar icons; the built-in apps (`app_manager_ui`, `package_manager_ui`) are the Applications and Package Manager sections
- Plugins are loaded from `~/Library/Application Support/Logos/LogosBasecampDev/plugins/`
- Session logging (stdout/stderr capture into `logs/`) is configured by an optional `config.yaml` in the session root — see the README's "Session logging" section; the code is `app/utils/LogSink.*` + `app/utils/LoggingConfig.*`
- Main UI is in `src/Basecamp/` (the main_ui plugin), organised by feature: `Sidebar/`, `Settings/`, `Shell/`, `Common/`, `Installer/`, `Icons/`

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

- **Confirmation intents**: the installer apps (App Manager, Package Manager) raise `basecamp.packages.confirm_install` / `confirm_uninstall` / `confirm_upgrade`, and only the default-app slots allowed to may (`ShellIntents::registerWith`: the slot's app and its backend); `beginPackageConfirmation()` draws the dialog and takes ownership of answering. The answer IS the permission — the requester removes nothing until it arrives, so the cascade-unload has always finished first. Basecamp installs nothing itself. Each confirmation is planned on a read of what is installed, dependencies included, made after it arrived (`afterFreshRead()`), so it never plans on a change still being read.
- **What an install brings**: the gate resolves the plan itself (`resolveDepChangesThen()`). Package Manager's requests keep what is installed where it satisfies the range, as each of its rows is only itself; App Manager's resolve every package to its newest catalog release (`installsNewestReleases()`), as it installs exactly the approved plan, so a dependency the catalog republished under the same version shows as the Reinstall it is and is all that is fetched (basecamp#427; lifecycle test B4). A request may carry the requester's picks, `versions` (name → version) and `optional` (name → bool) (`installChoicesFrom()`, `ResolverRequest.h`): the gate resolves with them as pins, the versions of what the plan needs fixed as picked, the optional packages starting as ticked and at the versions picked (an offer row's `requestedVersion`), still the person's to change (`withDialogPicks()`). App Manager sends what its details picked (lifecycle tests B5, B8).
- **Uninstall plans**: a `confirm_uninstall` is planned once the installed packages are read (`dependencyDataReady()`; until then the dialog shows its loading state). The `apps` slot removing one app removes with it what only that app needed (`removesAnApp()`, `planUninstall()`: `uninstallplan::composeFrom`), worded as an app. An approved uninstall answers `{names}`: what the requester may remove, and all it removes (basecamp-installer-common's `Installer`).
- **Bound consent**: the dispatch id lives on the thing it is answering (`PendingAction::intentRequestId`, or `m_pendingInstallRequestId` for the cascade-less install), never in a single "whatever is pending" slot. A click on one dialog must not be able to answer another request.
- **Undelivered answers**: `finishIntent()` reports whether the broker accepted. `false` means the requester is gone or the dispatch already ended — including when the cascade just tore down PMU itself — so the removal falls back to `performLocalRemoval()` rather than being silently dropped.
- **Cascade confirmation**: `confirmUninstallCascade`, `cancelPendingAction` — drives cascade unload via CoreModuleManager + UIPluginManager, then answers.
- **Trust at install** (`utils/InstallTrust.*`): the install gate and the upgrade dialog are told who signed every package its plan would bring — the catalogue's `signature` for the planned `rootHash`, or `verifyPackage` of a local file (PMU's `localPath`) — via `installTrustUpdated`, one prompt per install: the package the dialog is about gets its check/warning/cross mark (design-system icons) and the words ("Signed by Logos", `Common/InstallTrustLine.qml`), every other package just the mark beside its name (`Common/InstallTrustMark.qml`, same words on hover) and, when something is not trusted, a `LogosNotice` under the rows (`Common/InstallTrustNotice.qml`). Whether "Trust & install" is offered (`canTrust`) and whether both buttons say "anyway" (`trustCoversAll`) come from `InstallTrust::summary()`, not QML. The confirm calls refuse while that is pending or when anything is invalid; "Trust & install" adds one key to Basecamp's keyring (package_manager's `addTrustedKey`, in `<baseDirectory>/keyring`): the signer of the package the dialog is about (`summary()`'s `trust`), never a dependency's, whose packages install "anyway". It goes on only once a read of the keyring shows that key (`utils/PublisherKeyring.*`, which also owns Settings' deletes: one at a time, a row per key file, failures reported). Settings → Trusted Publishers only deletes, Logos included: a new keyring (no folder yet; the first keyring call makes it) gets Logos, retried on each read until a read shows it, and one Basecamp has used before never does, so a deleted Logos stays deleted. A keyring that can't be read trusts nobody: every signer reads as unchecked (`unchecked()`), nothing is offered for trust, and Settings says so. Logos's keys are a list (`logosKeys()`: the current key first, older ones still good, revoked ones); any Logos key in a keyring, old or revoked, stands for Logos and its keys now (`trustedDids()`), and a package signed with a revoked one is Invalid. Check the list before each release (README, "Cutting a release"). A refused or failed gate answers the requester `failed` and shows why (`installRefused`). The gate's answer holds the requester to what was judged: `plan` (`approvedPlan()`: each release pinned to its version, rootHash, repository and signer, and an unsigned one to the catalogue's SHA-256 of its file, since rootHash leaves its manifest out; nothing outside it installs) or, for a local file, `file` (`fileIdentity()`: rootHash, signer and the whole file's SHA-256, checked again right before the private copy made of the file installs). Both installer apps hold their installs to that approval with basecamp-installer-common (`Approval.h`: every required package fetched, nothing outside the plan, each file signed as approved, installed from a private copy). A confirm request is for a local file only when it names one (`requestsLocalFile()`), not when it has no repository; a local file Basecamp can't verify is blocked, and so is a signer key spelled other than liblgx writes it. What trusting allows is bounded: a package named like one Basecamp ships (its embedded modules and plugins) installs only if a Logos key signed it (`asReplacement()`, `reserved`), apart from Logos Storage (`updatableBundledModules()`), which the person may update like any package; an update signed by another publisher than the installed copy (the bundled copy is Logos's) asks and says so even when its signer is trusted, and is never offered for trust (`replacesSignedBy`); a name with a word that starts reading as Logos is Logos's (`claimsLogos()`); and the notice says what trusting does. A release installs under its manifest's name, so `fromCatalog()` blocks one whose catalogue entry names another package, or none (the downloader neither lists nor fetches the first, and holds every file to the name asked for). Replacing a copy no checkable key signed asks too, unless Logos signed it (`replacesUnverified`); what an install replaces is judged once the installed packages are known (`withInstalledAndTrustedKeys()`). The reserved names (`reservedNames()`) are the bundled modules (but `updatableBundledModules()`) and apps, each bundled app's `<app>_backend`, every name that may call package_manager, and `intents_module`; for each install, so is the backend (`<app>_backend`) of every ui_qml app installed or in the plan (`reservedFor()`), since an app's backend ships inside its app's package and a package by that name would make the backend unregistrable. A name that only ends in `_backend`, with no such app, is a package like any other: the catalogue's `eth_wallet_backend`, `monero_wallet_backend` and `uniswap_backend` install. They compare case-insensitively. The names Basecamp and the runtime call modules under (`identityNames()`: Basecamp's `core`, `core_service`, which the runtime lets call any module under an enforce policy, and the shell's `main_ui` and the link requester's `logos_link`, as intents_module knows them) never install either, Logos-signed or not, judged as Basecamp's own apps are. Every way in is judged by `asInstall()`: a catalogue plan's rows (the package, its dependencies, its upgrades) and a local file, an app whose `<app>_backend` an installed package has included (`backendTaken`). Basecamp's own apps (`utils/BuiltInApps.h`, the default-app table's; today App Manager and Package Manager, each app and its `<app>_backend`) never install, Logos-signed or not (`builtIn`), and run only from the bundle: a copy that the core's modules info or package_manager's app list shows in the user's dirs is moved into `<baseDirectory>/set-aside`, with a notice, once the core starts (`main.cpp`), after every `CoreModuleManager::refresh()` rescan and in `fetchUiPluginMetadata()`. Those scans list only the copy that wins, which is every copy that could run; an older one they hide stays put. A rescan repoints a loaded module's entry at the copy it found, so a loaded name is unloaded with its dependents before its copy moves. One that cannot be moved is refused by `preflight()` (`Problem::Kind::BuiltIn`) and by `UIPluginManager::loadUiModule()`. Standalone roots (`--module`, `--modules-dir`) are scanned as embedded, so they count as the bundle, and nothing is moved there (no user dirs). `nix build .#builtin-apps-test` checks all of it against the real core and package manager. Only Basecamp (`core`) and the installer apps' backends, `app_manager_ui_backend` and `package_manager_ui_backend` (`packageModuleCallers()`, from the default-app table), may call package_manager and package_downloader: `main.cpp`'s `restrictPackageModules()` registers that with capability_module right after the core starts, before either loads, the way `PluginLoader::restrictBackendToView` restricts an app's backend; one it can't restrict is not started, and an `--access-policy enforce` policy carries the same rule (`withRestrictions()`), since enforce mode would otherwise let any module that declares them as a dependency call them. That guards the call boundary only: a native module runs as the user and can write the keyring files directly. Every keyring call re-sends the keyring folder first. Promises and known limits: `/workspace/package-trust-v1.md`, "What trust promises". The installer's signature policy stays WARN, so this is the user's gate, not enforcement. The same words describe installed apps: the "Open with…" chooser's provider details (`MainUIBackend::providerDetailsFor` → `PackageCoordinator::installedTrust`, judged by `fromInstalled()`), and Settings → Trusted Publishers shows each key's "trusted since" date from the keyring's `addedAt`.
- **Metadata refresh**: `refresh()` triggers the full `getInstalledUiPlugins` + `getInstalledPackages` + per-package `resolveFlatDependencies/Dependents` chain; pushes UI metadata to UIPluginManager via `uiPluginsFetched` signal

### App-to-app intents (`app/IntentRegistry`, `app/IntentBroker`)
An app asks for a *capability* (`logos.request("wallet.send", …)`) and the shell picks who services it. Basecamp owns the **disposable** half — resolution, consent, dispatch; the frozen QML surface lives in `logos-view-module-runtime` (`LogosIntent.h`). No policy belongs in the frozen half, so this can be replaced wholesale when the core runtime takes over provider selection.

- **IntentRegistry** — reads `provides` / `uses` / `provides[].params` out of each installed app's `metadata.json`, through `utils/ManifestFile` as every reader of a package's `metadata.json` does (also `UIPluginManager::backendModuleOf` and the intents host's revision): a regular file only, never blocking on open (a FIFO there would hold the UI thread for good), and at most 1 MiB, as intents_module reads it; anything else is a diagnostic. `ui_qml` only, and that is a design line, not a V1 shortcut: core modules call each other directly through `LogosAPI` with no chooser and nothing to consent to. `logos.*` (the platform's) and `basecamp.*` (this shell's) are both reserved and refused from any on-disk record — the first by the frozen surface, the second by `IntentRegistry`, because which shell owns a prefix is policy.
- **IntentBroker** — the whole lifecycle. Reaches the world through four seams (`IntentEndpoint`, `IntentPresenter`, `IntentChooser`, `IntentInstaller`), all faked in `tests/intent_broker_test.cpp`, so the policy layer is testable with no UI.
- **IntentBridgeAdapter / ShellIntent\*** — bind each UI plugin's `LogosQmlBridge` to the broker, and re-emit chooser/install prompts as QML signals.

Load-bearing invariants, all covered by tests:

- **The requester's `requestId` never leaves its side.** The broker mints a separate `dispatchId`. A response is accepted only if the id is pending, the phase is `Dispatched`, **and** the responding endpoint is pointer-identical to the recorded provider — pointer, not name, so a reloaded app cannot inherit in-flight requests. A failed guard drops silently.
- **`unavailable` merges "nothing installed" with "denied"**, floored at 400 ms, so an app cannot enumerate what you have installed. The install suggestion answers the requester immediately and never completes its request, for the same reason.
- **One dialog at a time.** A second request queues rather than repointing a chooser under the user's cursor — that would be a consent swap.
- **Only a provider's own answer moves the user.** Answering returns them to the requester; the other five `finish()` paths (deadlines, endpoint death, abandon, refusals) never navigate, because nothing on screen would explain it. `"handoff": true` on a `provides` entry opts out entirely — the request existed to take the user somewhere and leave them there. It governs navigation only; when the provider answers (on arrival, or when the user marks the action done) is independent.

### intents_module presenter (`app/intents/IntentsModuleHost`)
Every build ships and runs `intents_module` for app backends and core modules; apps' screens keep the QML path above beside it until 0.5. `main.cpp` loads `intents_module` before anything else and claims it as the presenter at once, then hands the `IntentsModuleHost` to `Window` → `MainUIBackend`; if it fails to load, or can't be claimed (then it is unloaded: someone else may hold it), `HostProfile::intentsModule` goes false and only the QML path runs. `MainUIBackend::wireIntentsModule()` declares the shell (`ShellIntents::moduleDeclaration()`: the QML path's provides and hand-offs, every `confirm_*` restricted to the installer slots' backends, the link requester's web intents) and keeps `set_installed` current with apps and core modules (`pushInstalledToIntentsModule()`; an app's backend goes in under its app's display name and icon, read from the `<app>_backend/metadata.json` its app ships, whether or not it ran yet, unless an installed module has that name). The module knows modules, not apps: an app asks and answers through its backend (`<app>_backend`), which declares the app's `uses`/`provides` in its own `metadata.json` (logos-module-builder copies them there), and the app itself is only a name to the module. The host shows a backend as its app everywhere (`setAppOfModule` → `UIPluginManager::appOfBackend`). The catalogue stays in Basecamp: an `offer_install` names only the intent, and Basecamp finds candidates in its own registry (`installableProvidersFor()`; for a link answered `not_declared`, `installableFor()` with the link requester). The QML intent path keeps running beside it for apps not yet on backends.

- **When to interrupt** is Basecamp's call. Every new request comes as a `pending` item, opened by answering `open`. `bundled` (the default) opens one at once when the package asking shipped with Basecamp (`setRequesterBundled` → `PackageCoordinator::requesterIsBundled`, installed `embedded`; an app's backend is judged by its app's package), and any other only after a click or key press in the asking backend's app within 2 s, one request per click, so a core module's that doesn't ship with Basecamp never opens: it has no screen. A publisher you trust installs; it doesn't interrupt. `interactive` waits for that click for every request; `immediate` opens every one, and only builds with the QML inspector take it (`intentsAttentionFor()`; `--intents-attention` / `LOGOS_INTENTS_ATTENTION`, `HostProfile::intentsAttention`). What doesn't open is only logged (no pending-list UI yet; it expires `cancelled` after 30 min). The chooser names the requester's signer (`intentRequesterTrust`, from `providerDetailsFor`). `PluginLoader` watches every view of a module-backend app (`watchView`, rebuilt views included); only spontaneous input counts, except in builds with the QML inspector, whose clicks are posted.
- **One chooser on screen**, whichever broker asks: both feed `IntentChooserQueue`, which shows the next only once the dialog reports it closed (`intentChooserClosed`), and the dialog itself refuses to be repointed while open; install offers likewise don't replace an open one.
- **Work items** are pulled on `work_available` (queued, never inside the event callback), and once after wiring: `choose` → the existing chooser (`resolveIntentChooser`/`cancelIntentChooser` route by `ownsChoice()`), and the pick, only one of the providers offered (any other starts nothing and is answered `cancel`), is started before it is answered (an app through `UIPluginPresenter`, answered once its backend registers, or once it fails, comes up without one, or 45 s pass; a core module through `setProviderStarter`, as the Module Inspector starts it); one that didn't start fails in the module as `unavailable`. `present` → `UIPluginPresenter`, for a backend's app only; `deliver` (Basecamp provides) → `deliverShellIntent()`, the QML path's shell handlers, with the requester as its app, answered through `respondToShellIntent()` → `respondShell()`; a delivery whose request ends first (`dismiss`, a restart) → `shellDeliveryEnded` → `PackageCoordinator::shellIntentEnded()`, which closes its confirmation by request ID (`confirmationWithdrawn` → each dialog's `withdraw()`, which answers nothing: a cancel goes by package name and would hit a retry for the same package) (the QML broker does the same through `IntentEndpoint::dispatchEnded`); `completed` → auto-return with IntentBroker's guards, only for a dispatch that was presented, and never from an instance that has since gone; `offer_install` → the install offer, except for Basecamp's own intents. `declare_shell` and `set_installed` are sent again until the module takes them: an attempt ends at its first failed call (its 3 s timeout, when the module doesn't answer), and the next comes 1 s later, then twice as long each time, up to a minute, or at once when the module hands out work (`pullWork()`), so a click behind that work still counts. Every call to the module is asynchronous (`invokeRemoteMethodAsync` through the host's `Call`): nothing in Basecamp waits for one, so no nested event loop runs anything in the middle of another, except the claim in `main.cpp`, before any window exists. One sync step and one `next_work` are in flight at a time (a newer declaration or list goes next; a pull asked for meanwhile goes once the last is back), and what is on its way when the instance goes is answered by nobody (`m_inFlight`): Basecamp's own request fails, an answer to a shell delivery is not heard (`respondShell`'s `taken`), so `PackageCoordinator::finishIntent`'s `heard` falls back to `performLocalRemoval()`, and a late reply is dropped. Until the module holds both, Basecamp asks nothing through it and pulls none of its work (`isReady()`; what a restart's probe pulled waits too); a request of its own made while they are on their way waits for them to land (`m_asksWaiting`), since a link that launched Basecamp goes right after the first installed list, and fails if they are refused. `setInstalled()` sends a list only when it or a package's `metadata.json` changed, by its SHA-256, read as the module reads it (a regular file only, never blocking on open, at most 1 MiB), so an app upgraded in place is read again, whatever its file's time and size.
- **Basecamp's own requests** (`showPackageDetails`, `requestPackageInstall`) and `basecamp://` links go through `shell_request` alone (`IntentsModuleHost::shellRequest`; links via `LinkRequestCoordinator::setSubmitter`, as the link requester); their answers come back as `result` work items, to the same callbacks as before. When the module can't take one, or answers `unavailable` or `failed` (`askInstallerApp()`), Basecamp shows the app's section instead (Applications for an install, Package Manager for details), since the installer apps answer only through the module; a cancel is the person's answer. A `choose` for Basecamp's own request with exactly one provider is answered without a dialog once that provider is started, as IntentBroker skips it: the click in Basecamp was the choice. Only Basecamp can ask under its name, and a link asks under its own, so links and real choices are always shown.
- **Which app a backend is**: `UIPluginManager::appOfBackend` maps `<app>_backend` to its app only when that is the backend registered from the app's package; a module installed on its own under that name is never shown, clicked for or trusted as the app, and an app whose backend name an installed package has doesn't install, from the catalogue or a file (`asInstall()`'s `backendTaken`).
- **Backends**: `PluginLoader` lets `intents_module` call each `"backend": "module"` app's backend (`restrictBackendToView`) and tells the host once the view is up (`registerBackend`); `UIPluginManager::appBackendStopped` → `unregisterBackend`, which also drops an unspent click. A registered backend counts only while its app is loaded, so a pick of a closed app starts it again. The module is told none of it.
- **Restarts**: if `intents_module` stops, `keepIntentsModuleRunning()` loads it again (up to 3 times) and `restarted()` claims the new instance, replays the shell and the installed list, and only then logs "claimed again after it restarted"; the old instance's dialogs close and picks waiting for an app are dropped. A false alarm keeps the old claim. Past the third, or when a reload fails, the host drops the claim (`forgetInstance()`), so nothing more is sent to it; a later stop notice may try a failed reload again, within the three, and the log says whether it will. A new instance Basecamp can't claim is unloaded and not loaded again (`presenterRoleLost`). The claim goes to the first host caller, so a restart is a window: tracked until the core can give the presenter a credential. It is locked in the Module Inspector.
- Tests: `tests/intents_module_host_test.cpp` (fake module), `tests/intent_chooser_queue_test.cpp`, `nix build .#intents-module-test` (in CI; runs `interactive`, to cover the pending list and opening on a click; requester and providers in `tests/fixtures/intents-module/`, each binding the module by `contracts/intents.lidl` as the guide shows; override `logos-intents-module` (and its `logos-module-builder`) with `path:../<repo>` to test local changes).
- The dev builds can install nothing from the catalogue (it ships portable variants only); for catalogue installs, `nix run .#bin-bundle-dir-inspector`.

### Deep links (`app/links/`)
A `basecamp://` URL from a browser becomes an intent. `SingleInstanceGuard` (socket keyed on the **resolved** user dir, so `--user-dir` still isolates; `--new-instance` skips `acquire()` entirely, and the secondary path prints why it exited rather than returning 0 in silence — README, "Parallel Instances"), `LinkUrlInbox` (where argv, the macOS `QFileOpenEvent` and the socket all land), `LinkUrl` (the parser — pure, and the only fully attacker-controlled surface), `LinkRequestCoordinator` (parks until the first registry rebuild, caps one in flight), `SchemeRegistrar` (Linux `.desktop` + Windows `HKCU`; macOS is the plist).

Two invariants, both tested: a link submits under **its own** requester name, never `main_ui`, or the broker would skip the chooser and every web link would dispatch with no consent; and an app's capability is unreachable from a URL until its author sets `"web": true` on the `provides` entry.

Where things are: dialogs in `src/Basecamp/Shell/Intent*Dialog.qml`, wiring in `Shell/OverlayDialogs.qml`, fixtures in `tests/fixtures/intents/`. Full design and known limitations: `docs/app-to-app-intents.md`.

### Host profile and standalone mode (`app/utils/HostProfile.h`, `app/utils/StandaloneMode.h`)
`main.cpp` builds one `HostProfile` and passes it down `Window` → `MainUIBackend` → `UIPluginManager` / `PackageCoordinator`. Components read capabilities, never "is this standalone". `StandaloneMode` only builds the dev-host profile from `--module` (install roots only — a single module dir is refused, it would expose its siblings) `--modules-dir` and `--qml-source` (into `qmlSources`).
- `main.cpp`: scan roots, user modules dir, `package_downloader`, URL scheme, single instance, default user dir and application name — all from the profile.
- `PackageCoordinator`: profile roots via `addEmbedded*Directory`; user dirs only if `useUserModules`. The downloader subscription depends only on whether the downloader is loaded.
- `PackageCoordinator`: `enforceDependencyVersions` (false in standalone) — `splitDependencyRows` moves a version mismatch out of the blockers into `ignoredMismatches`, logged as a warning. Missing and signer-mismatched dependencies still block.
- `UIPluginManager`: `openStartupApps()` opens `appsToOpen` once dependency data is ready; `launcherApps()` filters when `launcherLimitedToOpenedApps`; an app in `qmlSources` gets its `qmlViewPath` from the source tree and a `hotReloadDir`.
- `PluginLoader` / `QmlHotReloadView` (`app/QmlHotReload.h`): with `hotReloadDir`, the app's tab is a `QmlHotReloadView` that owns the `QQuickWidget` and replaces it on save. The old view and engine are deleted before the new one is built — a surviving engine keeps `.pragma library` JS and `.mjs` stale. The bridge moves to the new engine; `WorkspaceArea` finds the view as the container's direct `QQuickWidget` child.
- The shell reads `backend.availableSections` (Sidebar, `MainContainer`'s redirect), `repositoryManagement` (Settings), `packageCatalog` (WelcomePage) and `devHost` (sidebar label). A backend without them (shell-preview) gets the full shell. A default app with a section (the default-app table; `package_manager_ui` is the Package Manager section) is hoisted into it wherever that section is offered (`HostProfile::sectionOfApp`, checked by `UIPluginManager::isShellInternalApp`, `UIPluginPresenter`, `MainContainer` and the sidebar through `backend.sectionApps`): always on desktop, and in standalone mode when it is one of the `--module` apps (then selected on arrival). Loaded otherwise, it is docked.
- `nix/dev.nix` (`.#dev`, what logos-module-builder's `nix run .` / `ui-dev` / `ui-tests` launch): `--dep-package` (ready-built .lgx, installed as-is)/`--dep`/`--app` → `nix/devDeps.nix` installs the dependencies (one copy per module, first reached wins) → `LogosBasecamp --module … --modules-dir …`; `--test` runs .mjs files headless instead. `mkPluginTest` takes the same `deps`.
- `flake.nix`: `app-standalone` (dev) and `app-standalone-portable` (portable, inspector on) bundle only what `--module` loads; `lib.<system>.mkPluginTest`; checks `standalone-test`, `standalone-backend-test`, `standalone-module-backend-test` (module backend: isolation, intruder, takeover refusals, Inspector, crash + Reopen, unload on close), `standalone-rejects-module-dir`.

### Default apps (`app/interfaces/DefaultApps.h`)
Basecamp's default apps by slot, in one table the host and the shell both read: the slot (`apps` → `app_manager_ui`, `packages` → `package_manager_ui`), the section the app is shown as, the confirm intents only that slot may raise, whether it may name a local .lgx (Package Manager only), whether its backend may call the package modules, and the intents Basecamp's own requests go to it for without a chooser (`defaultFor`: `packages.install` and `apps.search` to App Manager, `packages.show` to Package Manager; `IntentsModuleHost::setDefaultProvider`). Grants go by slot, never by a name spelled out elsewhere: `BuiltInApps::apps()`, `ShellIntents::registerWith()`'s and `moduleDeclaration()`'s restrictions (to the slots' backends, for intents_module), `packageModuleCallers()`, `HostProfile::sectionOfApp()`, `MainContainer`'s section pages and the local-file check all derive from it, and `tests/default_apps_test.cpp` holds them to it.

### One package change at a time (`app/ConfirmationQueue`)
Every `basecamp.packages.confirm_*`, from either broker, goes through `ConfirmationQueue` before `PackageCoordinator::beginPackageConfirmation`: one dialog on screen, and once one is approved the next waits until that change is on disk, each required package at its approved release, root hash included (`PackageCoordinator::installedPackagesRead`; an optional package may fail and be left out), the app that made it says how its request ended (App Manager's `finished`, Package Manager's `installationProgressUpdated` Completed / ProgressFailed: `UIPluginManager::installerChangeEnded`), asks again (an installer app asks only after its last change ended), closes or stops, or two minutes pass with nothing of it moving (package_downloader's `downloadProgress` / `downloadDone` for a package in it, `PackageCoordinator::packageOnItsWay`; any files package_manager installs or removes, `packageFilesChanged`). A request that ends while waiting leaves the queue.

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
| `src/Basecamp/Settings/AppearanceView.qml` | Appearance: theme picker, "Add custom theme…" and Remove; installs and switches via the `themeManager` context property (the design system's `Logos::ThemeManager`, set on the content view only in `MainContainer.cpp`); the view only emits signals |
| `src/Basecamp/Shell/ContentViews.qml` | Settings (Repositories, Apps/Module Inspector, Appearance, …) and the shell's own intents; Applications and Package Manager are their apps (`app_manager_ui`, `package_manager_ui`), each a page of `MainContainer`'s stack |

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

- `src/Basecamp/` - QML UI source files, organised by feature (Sidebar/Settings/Shell/Common/Installer/Icons)
- `src/Basecamp/Installer/` - a copy of basecamp-installer-common's JavaScript, the words and rules the two installer apps and the shell's install dialogs share (`import Basecamp.Installer`). Change it there, then `scripts/sync-installer-common.sh`. The `shared-installer-words` check fails on a copy that isn't the pinned revision, and on a shared phrase written again in the shell's QML (exceptions, with reasons, in `tests/shared-words-allow.txt`)
- `nix/` - Nix build configurations (app.nix, smoke-test.nix, integration-test.nix)
- `logos-qt-mcp` - QML Inspector: MCP server, test framework, Qt plugin (separate repo, flake input)
- `tests/` - UI integration tests
- `qt-ios/` - iOS build scripts
