# evil_app — adversarial ui_qml sandbox-escape fixture

This is **not** a real app. It is the evil twin of [`repos/counter_qml`](../../../../counter_qml):
where counter_qml is a benign panel whose escape attempts a human triggers by clicking buttons, this
app fires every escape vector **automatically on load** and tallies how many got through, so a
headless test can assert the result.

It is consumed by the C++ regression test `tests/sandbox/tst_qml_sandbox.cpp`, which loads `Main.qml`
through the real production sandbox (`src/restricted/QmlSandbox.cpp`, the same code path
`PluginLoader::loadQmlView` runs) and asserts `escapes === 0`. This mirrors how
[`tests/sandbox/evil_plugin`](../evil_plugin) is a fixture for the native-plugin escape — except this
one is **QML-only** (no compiled `.so`).

## Files

| File | Purpose |
|------|---------|
| `Main.qml` | Pure-QtQml view; on load runs all probes and exposes `escapes` / per-probe status properties. |
| `EvilModule/qmldir` | Declares a native `plugin` but ships no `.so` — the QML-only F-008 probe. The sandbox must reject this qmldir. |
| `evil_import.qml` | `import EvilModule` — must fail to compile under the sandbox (rejected qmldir). |
| `metadata.json` | Marks it a `ui_qml` module so it *can* be hand-copied into a basecamp plugins dir for a manual demo. |

## Probe → counter_qml mapping

| counter_qml probe | evil_app vector | Sandbox mechanism |
|---|---|---|
| `XMLHttpRequest GET https://…` | `httpStatus` | deny-all NAM |
| `XMLHttpRequest GET file:///etc/hosts` | `fileXhrStatus` | deny-all NAM |
| `Qt.openUrlExternally(file:///etc/hosts)` | `openUrlStatus` | `interceptUrl(UrlString)` → empty |
| `Loader.source = http://…/fake.qml` | `remoteCompStatus` | interceptor blocks remote scheme |
| `Image.source = file:///etc/hosts` | `outsideFileCompStatus` | interceptor blocks file outside roots |
| (F-008 native plugin) | `EvilModule` import | interceptor rejects qmldir declaring a plugin |

The counter_qml `QFile` probe is intentionally omitted: `QFile` is not a registered QML type, so it is
absent by construction rather than a sandbox guarantee to assert.

## Run

```bash
cd repos/logos-basecamp
nix build .#sandbox-test -L   # builds + runs every sandbox ctest slot, incl. the evil_app ones
```
