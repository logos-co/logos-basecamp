# Mock backend

Run Basecamp with no Logos module runtime: no `liblogos_core`, no `logos_host`,
no module subprocesses. Module lists, packages and stats come from a JSON
fixture.

```bash
nix build .#app-mock && ./result/bin/LogosBasecamp
```

The sidebar reads `Dev build (Mocked)` in the warning colour, so a fixture build
is never mistaken for a working one.

On startup you should see:

```
MockBackendFixture: ... — LOGOS_MOCK_FIXTURE exported
MockBackendFixture: SDK mode is Mock — no IPC will be attempted.
MockStore: seeded 12 canned call(s) from ...
```

If the second line instead **warns** that the mode is Remote, stop and read
"One logos-protocol" below — nothing downstream will work and the failure is
otherwise silent.

## How it works

Basecamp codes against `app/interfaces/ICoreRuntime.h`, an interface it owns.
Two implementations:

| | implements `ICoreRuntime` | compiled when |
|---|---|---|
| `app/QtLogosCoreRuntime.cpp` | the real runtime | `LOGOS_USE_MOCK_BACKEND=OFF` |
| `mock/src/FixtureCoreRuntime.cpp` | the fixture | `LOGOS_USE_MOCK_BACKEND=ON` |

`QtLogosCoreRuntime` is the only file in the app that names the core, and it is
not compiled at all in a mock build.

`MockBackendFixture` resolves the fixture's placeholders, writes the result under
the user data dir, and exports `LOGOS_MOCK_FIXTURE`. Everything else keys off
that variable: logos-protocol reports `LogosMode::Mock` while it is set, and
`MockStore` seeds itself from the file it names. Child processes inherit it.

## One logos-protocol, in every image

`LogosModeConfig` and `MockStore::instance()` are header-only inline, so **every
image compiles its own copy** — the app, each shared library, `ui-host`, and each
plugin that links logos-protocol statically. Each reads `LOGOS_MOCK_FIXTURE` for
itself; a `setMode()` call in one image cannot reach another.

That only works if every image is built from a logos-protocol that *has* the
env-var read (logos-protocol#76). An older revision has no such code, so that
image is permanently Remote no matter what the parent does. Five images matter:

```
lib/liblogos_protocol.{so,dylib}   lib/liblogos_qt_host.{so,dylib}
bin/ui-host                        bin/LogosBasecamp
plugins/<app>/<app>_plugin.{so,dylib}
```

Check them:

```bash
for f in lib/liblogos_protocol.so lib/liblogos_qt_host.so bin/ui-host \
         bin/.LogosBasecamp plugins/package_manager_ui/package_manager_ui_plugin.so; do
  printf '%-56s %s\n' "$f" "$(nm -C result/$f 2>/dev/null | grep -c modeExplicitlySet)"
done
```

Every one must print `1`. A `0` means that image's dependency chain still pins an
old logos-protocol. Note the app's compile-time `-DLOGOS_PROTOCOL_ROOT` does
**not** reach the others: the runtime `liblogos_protocol` is staged from
`${logosLiblogos}/lib`, `ui-host` comes from logos-view-module-runtime, and a
plugin comes via logos-module-builder. Each is bumped in its own repo, or pinned
together with `inputs.logos-protocol.follows` in `flake.nix`.

Symptom when one is stale: that image logs `RemoteTransportConnection`, dials
modules that do not exist, and burns a 20s timeout per call inside
`QEventLoop::exec(ExcludeUserInputEvents)` — so the UI paints but ignores input.

## Scope

Mocked: the module runtime, and the LogosAPI calls Basecamp and plugin backends
make.

**Not mocked: ui_qml hosting.** `ui-host` is still staged and still spawned, and
a plugin's library is still `dlopen`ed. A ui_qml app's backend is real code; only
the module calls it makes are answered from the fixture.

Does not work: install, uninstall and upgrade complete no round trip, and the
`package_manager` event subscriptions never fire (`MockLogosObject::onEvent` is a
no-op).

## How a plugin's list actually gets filled

Worth understanding before changing anything, because the data does not come from
the fixture directly. PMUI's QML binds a **QtRO model replica**:

```qml
property var backend: logos.module(moduleName)                    // replica
property var packagesModel: logos.model(moduleName, "packages")   // model replica
```

`MockStore` only answers `callMethod`; it knows nothing about models. The chain
is:

```
fixture ──► MockStore in ui-host's image
              │
              ▼
       the REAL plugin backend (PackageManagerBackend)
              │  populates its own `packages` model
              ▼
       remoted over QtRO ──► logos.model(...) ──► the QML list
```

So the list is only ever as good as the backend's own calls being answered. If
the list is empty, the backend is not in Mock mode or a method it calls has no
fixture entry — look at `ui-host`'s output (below), not at the QML.

## The fixture

`fixtures/mock-backend.json`, compiled in as a Qt resource.
`LOGOS_MOCK_FIXTURE=<path>` overrides it. The resolved copy written at startup can
be edited and reloaded without rebuilding.

Invariants:

- **`package_manager` must be `"loaded": true`.** `PackageCoordinator.cpp:100`
  gates its directory setup and every event subscription on it.
- **`capability_module` must appear under `calls`,** even though Basecamp never
  calls it directly. `MockStore`'s seeder mints a `TokenManager` token for every
  module named by a key, and the host must hold a `capability_module` token to be
  admitted as the trusted channel when loading a ui_qml plugin. Without it:
  `logos::admitConsumer: the host holds no capability_module token`.
- **`calls` keys use the wire method name** — the generated wrapper's name minus
  any `Async` suffix.

Placeholders expanded at load: `{PLUGINS_DIR}`, `{MODULES_DIR}`, `{APP_DIR}`,
`{LIB_EXT}`. Do not hardcode a library suffix — `UIPluginManager::hasBackendPlugin`
accepts `.so`/`.dylib`/`.dll` interchangeably, so a wrong one passes that gate and
fails later in `ui-host`.

Rows are dropped silently if a `ui_qml` entry has no `view`, or a non-`ui_qml`
entry has no `mainFilePath` (`UIPluginManager::onUiPluginsFetched`).

## Debugging

`ui-host` runs in its own process and **its output is discarded by default** —
`ViewModuleHost` forwards the child's stderr through the `logos.viewhost`
category, which is declared `QtWarningMsg`. Everything the plugin backend logs,
including its `MockStore` hits and misses, is invisible until you ask for it:

```bash
QT_LOGGING_RULES='logos.viewhost.debug=true' ./result/bin/LogosBasecamp
```

Then look for `ui-host <module> : ...` lines. `no expectation registered for
"<module>" :: "<method>"` names exactly what the fixture is missing.

## Adding a UI app

1. add the UI repo as a flake input
2. add its plugin to `installedDev` in `flake.nix`
3. add fixture rows: `getInstalledUiPlugins`, `getCatalog`, and its module's calls
4. confirm its plugin image passes the `nm` check above

No change to the app itself — its backend runs unmodified and its module calls
resolve from the fixture.

## Tests

```bash
nix build .#mock-tests -L          # or: ./mock/tests/run-standalone.sh
```

Add `nix build .#mock-tests -L` to CI explicitly; nothing here runs
`nix flake check`.

## Mobile

This removes the need to port the Logos runtime, not the need to port the plugin
machinery — `ui-host` is a subprocess and plugins are `dlopen`ed, neither of which
survives on iOS. See `../MOBILE-HANDOFF.md`.
