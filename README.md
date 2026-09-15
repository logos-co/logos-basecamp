# logos-basecamp

## Download

Prebuilt binaries for Linux (AppImage) and macOS (DMG):

- **Stable release** — [latest release](https://github.com/logos-co/logos-basecamp/releases/latest)
- **Latest RC / development build** — [all releases](https://github.com/logos-co/logos-basecamp/releases)

Release candidates are marked as pre-release, so `/releases/latest` skips them. If the stable release is significantly older than the RCs on the releases page, you probably want an RC.

## How to Build

### Using Nix (Recommended)

#### Local Build

The local build produces a standard Nix derivation whose dependencies live in `/nix/store`. It is the fastest way to iterate during development but is **not portable** — it only runs on the machine that built it.

```bash
nix run .
# or: nix build '.#app' && ./result/bin/LogosBasecamp
```

Local builds require **local** `.lgx` packages, generated with:

```bash
nix bundle --bundler github:logos-co/nix-bundle-lgx github:your-user/your-module#lib
```

#### Portable Builds

Portable builds are fully self-contained — no `/nix/store` references at runtime. They work with **portable** `.lgx` packages. That is, releases from [logos-modules](https://github.com/logos-co/logos-modules), downloads from the Package Manager UI, or generated with:
```bash
nix bundle --bundler github:logos-co/nix-bundle-lgx#portable github:your-user/your-module#lib
```

| Output | Platform | Format |
|---|---|---|
| `bin-bundle-dir` | Linux, macOS | Flat directory with `bin/` and `lib/` |
| `bin-appimage` | Linux | Single-file `.AppImage` executable |
| `bin-macos-app` | macOS | `.app` bundle (ad-hoc signed, unsigned for distribution) |

##### Self-contained directory bundle (all platforms)
```bash
nix run .#bin-bundle-dir
# or: nix build '.#bin-bundle-dir' && ./result/bin/LogosBasecamp
```

##### Linux AppImage (Linux only)
```bash
nix build '.#bin-appimage'
./result/logos-basecamp.AppImage
```

##### MacOS App bundle (macOS only)

```bash
nix build '.#bin-macos-app'
open result/LogosBasecamp.app
```


#### Mock Backend (no Logos runtime)

Runs Basecamp against a JSON fixture instead of a live module runtime: no `liblogos_core`, no `logos_host`, no module subprocesses. Useful for working on the UI while the core is unavailable, and as a starting point on platforms the runtime has not been ported to.

```bash
nix build '.#app-mock' && ./result/bin/LogosBasecamp
```

The sidebar reads `Dev build (Mocked)` in the warning colour, so a fixture build is never mistaken for a working one. Module lists, packages, the catalog and stats all come from `mock/fixtures/mock-backend.json`.

| Output | What it is |
|---|---|
| `app-mock` | Local mock build |
| `bin-bundle-dir-mock` | Portable mock bundle |
| `mock-tests` | Fixture and placeholder-resolution tests |

UI plugins still load and run their **real** backends — only the module calls those backends make are answered from the fixture. `ui-host` is still spawned and plugins are still `dlopen`ed; the mock replaces the core, not the plugin machinery. Install, uninstall and upgrade complete no round trip, and module events never fire.

Two things to know before changing anything: every image that talks to a module carries its own copy of the mode flag, so all of them must be built from a logos-protocol that reads `LOGOS_MOCK_FIXTURE`; and `ui-host`'s output is hidden unless you run with `QT_LOGGING_RULES='logos.viewhost.debug=true'`. Both are covered in [`mock/README.md`](mock/README.md), along with the fixture invariants.

#### UI-only Preview (no Logos code at all)

If you only want the shell UI — to work on layout, or as a first bring-up on a new platform — [`shell-preview/`](shell-preview/README.md) loads the real `main_ui.so` through `IShellHost` with fixture data and links no Logos library whatsoever.

```bash
nix run .#shell-preview
```

Unlike the mock build it ships no `liblogos_protocol`, spawns no `ui-host` and loads no plugins — its nix closure contains no Logos library at all, so a core rework cannot reach it. For mobile, see [`MOBILE-HANDOFF.md`](MOBILE-HANDOFF.md).

#### Parallel Instances (`--user-dir`, `--new-instance`)

**One Basecamp per data directory.** Start a second one against a directory
that is already in use and it will not open a window: it hands over any
`basecamp://` URL it was launched with, asks the running window to come
forward, and exits 0. From a terminal that looks like nothing happened, so it
says so on stderr:

```
Basecamp is already running for /Users/you/Library/Application Support/Logos/LogosBasecampDev; asked it to come forward.
Use --new-instance to start a second one anyway, or --user-dir <path> for an isolated instance.
```

If you see that and no window appears, the first instance is probably on
another desktop or minimised — or it is wedged, in which case check for a
leftover process (`ps aux | grep LogosBasecamp`).

This guard exists for deep links. On Linux and Windows the OS answers a
`basecamp://` click by launching the registered handler as a **new process**
with the URL in argv, whether or not Basecamp is running; without the guard
every click would start another runtime against the same `plugins/` and
`module_data/`. macOS delivers to the live process instead and does not need
it, but it runs there too so a terminal launch behaves the same everywhere.

There are two ways to get a second instance.

**`--user-dir <path>` (or `-u`) — isolated.** Sets the base directory, giving
the instance its own `plugins/`, `modules/`, `module_data/` and `logs/`. The
guard's socket name is derived from the resolved path, so a different directory
is a different guard. The path is used verbatim; equivalent to setting the
`LOGOS_USER_DIR` env var.

```bash
# Two instances with isolated state
./result/bin/LogosBasecamp --user-dir /tmp/basecamp-a &
./result/bin/LogosBasecamp --user-dir /tmp/basecamp-b &
```

A fresh directory starts with no apps installed. For a second instance that
looks like your real one, copy the session directory first and point
`--user-dir` at the copy.

**`--new-instance` — shares the directory, and gives up the guarantee.** Skips
the guard entirely and runs against the directory already in use. Two runtimes
then load the same plugins and can both install packages into the same
`module_data/` — exactly what the guard prevents — so this is a development
escape hatch, not a normal way to run.

```bash
./result/bin/LogosBasecamp --new-instance
```

The first instance keeps the socket, so a `--new-instance` process is
**link-deaf** on Linux and Windows: clicked `basecamp://` links keep going to
whichever instance started first.

#### Session logging (`config.yaml`)

Everything Basecamp and its module hosts write to stdout/stderr goes to a
rotating file under `logs/` in the session directory. Nothing needs configuring
for that to happen; a `config.yaml` in the root of the session directory changes
how it behaves:

```
~/Library/Application Support/Logos/LogosBasecamp/     # macOS ("…Dev" for a dev build)
~/.local/share/Logos/LogosBasecamp/                    # Linux
├── config.yaml
├── logs/
├── modules/
├── module_data/
└── plugins/
```

```yaml
logging:
  enabled: true          # false -> no log file at all
  file: basecamp.log     # the log's name, inside dir
  dir: logs              # relative -> inside the session; also ~/x or /var/log/x
  max_size_mb: 10        # rotate past this; 0 = never rotate
  max_files: 5           # keep this many in total, oldest dropped
  console: true          # also mirror to the terminal that launched the app
```

`--user-dir` moves the session, and the document with it, so parallel instances
configure their logging independently.

Each launch writes a **new, timestamped file**, and the name in `file` survives
as a symlink to whichever one is current — so `tail -F logs/basecamp.log` follows
across restarts without anyone working out a stamp:

```
logs/
├── basecamp.log -> basecamp_20260909_180411.log   # always the current session
├── basecamp_20260909_180404.log
└── basecamp_20260909_180411.log
```

`max_files` bounds the **directory**, not just one launch's rotations: the oldest
files are pruned at each start, so an app started a hundred times does not leave
a hundred logs behind. Earlier builds kept every file forever, so the first
launch after upgrading prunes whatever those left behind — raise `max_files`
first if you want to keep them.

Capture is pipe-based rather than a file redirect, which is what lets a module
host's output land in the log: those are separate processes holding inherited
descriptors, and renaming a file out from under a child that still has it open
just keeps filling the old inode.

A document that cannot be read is reported and then **ignored entirely** —
logging falls back to the defaults rather than to some half-applied mixture of
the two. The reason is written into the log file itself as well as to the
terminal, so `logging.max_size_mb: expected a whole number, but got a string.`
is there to find either way. Unknown keys are named too, since a `max_size` that
was silently dropped looks exactly like one that was honoured.

This is the same mechanism, spelling and file layout as `logosctl`'s daemon
logging; see that tool's `docs/logosctl.md` for the daemon-side view.

#### Inter-module access enforcement (`--access-policy`)

**Default: off.** Without this flag, any loaded module may call any other —
unchanged behaviour.

`--access-policy enforce` turns on **deny-by-default**: a module may only call
the modules it declares as dependencies in its `metadata.json`, and any other
call is refused before it can proceed.

```bash
./result/bin/LogosBasecamp --access-policy enforce
```

The startup log states which side it landed on, so a policy that failed to arm
is visible rather than silently permissive:

```
Installing inter-module access policy: {"version":1,"mode":"enforce","restrictions":{}}
Inter-module access enforcement is ON (mode=enforce): deny-by-default — ...
```

and a refusal names **both** modules, so a denial never presents as a
mysteriously empty result:

```
[capability_module] access policy denies 'caller_module' -> 'target_module'
```

> **Known limitation — read before enabling.** UI plugins (`ui_qml`) load
> out-of-process and are **not** tracked as dependents in the core module
> registry, so the derived allow-list never contains them and their calls to
> their own backend module get denied (e.g. `accounts_ui -> accounts_module`).
> That is why this is off by default. Until the derivation accounts for
> `ui_qml` callers, name them explicitly with a policy document.

`--access-policy` also accepts a path to a JSON policy file, or inline JSON,
where a `restrictions` entry **replaces** the derived allow-list for that
target:

```bash
./result/bin/LogosBasecamp --access-policy ./policy.json

./result/bin/LogosBasecamp --access-policy \
  '{"version":1,"mode":"enforce","restrictions":{"accounts_module":{"allowedCallers":["accounts_ui"]}}}'
```

`mode` is the switch — only `"enforce"` activates gating, and `enforce` is
shorthand for exactly `{"version":1,"mode":"enforce","restrictions":{}}`. An
unreadable file or malformed JSON aborts startup rather than booting wide open.
Equivalent to setting the `LOGOS_ACCESS_POLICY` env var (the flag wins), which
is the way in for a launch with no argv — a double-clicked bundle or a desktop
entry. The same flag and spellings work on the `logoscore` CLI.

#### Development Shell

```bash
nix develop
```

**Note:** In zsh, quote the target (e.g., `'.#app'`) to prevent glob expansion.

If you don't have flakes enabled globally:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

#### Nix Organization

The nix build system is organized into modular files in the `/nix` directory:
- `nix/default.nix` - Common configuration shared by every derivation
- `nix/app.nix` - The application build
- `nix/main-ui.nix` - The `main_ui` UI shell plugin

## App-to-app intents

Apps ask for a *capability*, not for each other. A chat app can offer "send funds" the day a wallet is installed — no release, no dependency, no agreement between the two teams beyond the name `wallet.send`:

```js
logos.request("wallet.send", { to: "0xabc", amount: 12.5 }, function (res) {
    if (res.ok) console.log("sent", res.data.txHash)
    else        console.log("did not happen:", res.error)
})
```

Basecamp resolves the capability to an app that declared it, asks you which one, brings it forward, and routes the answer back to the caller alone. The calling app never names a provider and never learns what you have installed.

Declare capabilities in your app's `metadata.json` — `provides` for what it can service, `uses` for what it may request.

**Nothing is signed yet**, so a provider's name and label are claims rather than identity. Read `docs/app-to-app-intents.md` before depending on this.

## Modules

### Blockchain

The *Blockchain App* lets you run your own blockchain node.

The app currently supports:
1. Joining the Logos Testnet
2. Participating in consensus (chain-following and proposing blocks)
3. Making and receiving transfers

You can run the Blockchain App through the Logos Basecamp, or standalone by building and running the app from source, instructions [here](https://github.com/logos-blockchain/logos-blockchain-ui?tab=readme-ov-file#how-to-build).

### LEZ Wallet

The *Logos Execution Zone Wallet App* lets you interact with the Logos Execution Zone (LEZ). It is currently limited to basic account operations. This showcases both private and public execution through RISCV emulation and ZK Proofs.

The wallet currently supports:
1. Initializing private/public accounts
2. Inspecting private/public account balances
3. Public to Public transfers
4. Private to Private transfers

You can run the LEZ Wallet through Logos Basecamp, or standalone by building and running from source, see instructions [here](https://github.com/logos-blockchain/logos-execution-zone-wallet-ui?tab=readme-ov-file#how-to-build).

### Storage

The Logos Storage App allows you to publish, download, and share files with other Logos users. You can run it both in standalone mode, or as part of the main Logos Basecamp.

Sharing files requires direct connection across nodes, so you will need to set up your router to allow NAT traversal either via UPnP, or manual port forwarding. The app will help you figure out if your NAT traversal is working with a reachability check.

Check [the app's README file](https://github.com/logos-co/logos-storage-ui) for more information on how to build, run, and troubleshoot it.

### Chat

The Logos Chat App lets you send and receive private 1:1 messages, where messages are transferred over Logos Delivery, the decentralised transport layer. You can run it both in standalone mode, or as part of the main Logos Basecamp.

In the current testnet demo, the app supports:
- Creating and sharing your intro bundle (a contact identifier others can use to reach you)
- Starting private conversations by pasting a counterparty's intro bundle
- Sending and receiving messages in real time

To start a conversation, share your bundle with another user (via the "Share Bundle" button), and paste theirs into the new conversation dialog.

Check [the app's README](https://github.com/logos-co/logos-chatsdk-ui) for instructions on how to build and run it in standalone mode.

### Mix Demo Chat

You can use the Mix Demo Chat app to send anonymous chat messages over the mixnet.
This demonstrates two core primitives working end-to-end:
- Decentralised discovery with capability filtering
- Anonymous message routing over the mixnet

Your chat client will first discover the necessary addresses and keys for a pool of mix nodes (using the [capability discovery API](https://lip.logos.co/ift-ts/raw/extended-kad-disco.html#api-specification)) and then proceed to route every published message through this libp2p [mix overlay network](https://lip.logos.co/ift-ts/raw/mix.html).

You can run Mix Demo Chat inside the Logos Basecamp.
On loading, the UI will show the following:
- Status is shown as *Ready*
- LP Peer count increasing over time before stabilising
- Mix peer count increasing over time before stabilising
- Warning message `Waiting for network peers...` disappears once 3 mix nodes are discovered

Once the warning message disappears, you can send messages, which will be receivable by others running the app.
Sent messages appear in the `Messages` section of the UI once they have been successfully delivered over the mix layer.

Mix Demo Chat can also be run as a standalone app.
To do so, or for more information, refer to the module repo and instructions [here](https://github.com/logos-co/logos-chat-legacy-ui/tree/logos-testnet-mix-demo).

## Testing

### Smoke Test

Validates the app starts without QML errors or crashes:

```bash
nix build .#smoke-test -L
cat result/smoke-test.log
```

### Mock Fixture Tests

Covers the fixture's invariants and the startup placeholder resolution. Not part of `nix flake check` — run it explicitly in CI:

```bash
nix build .#mock-tests -L
```

### UI Integration Tests

End-to-end tests that open apps, click buttons, and verify visible text using the [QML Inspector](https://github.com/logos-co/logos-qt-mcp).

**Run via Nix** (fully hermetic, suitable for CI — no Node.js or npm required):

```bash
nix build .#integration-test -L
cat result/integration-test.log
```

**Run with Node.js** (requires Node.js and a built app):

```bash
# Build the app and test framework (one-time):
nix build
nix build .#logos-qt-mcp -o result-mcp

# Run headless (launches the app, runs tests, kills the app):
node tests/ui-tests.mjs --ci ./result/bin/LogosBasecamp

# Or run against an already-running app:
node tests/ui-tests.mjs

# Run a subset:
node tests/ui-tests.mjs modules
```

Tests are defined in [`tests/ui-tests.mjs`](./tests/ui-tests.mjs) using the test framework from [logos-qt-mcp](https://github.com/logos-co/logos-qt-mcp). See the [logos-qt-mcp README](https://github.com/logos-co/logos-qt-mcp#readme) for the full test API.

### AI Agent Interaction (MCP)

An MCP server allows AI assistants (Claude, etc.) to interact with a running instance of the app — inspecting the UI, clicking elements, reading properties, and taking screenshots.

Build the logos-qt-mcp package (one-time, includes the MCP server, test framework, and Qt plugin):

```bash
nix build .#logos-qt-mcp -o result-mcp
```

The `.mcp.json` in this repo is pre-configured to use the MCP server from `result-mcp/mcp-server/`. Start the app (inspector is enabled by default in debug/dev builds), and the agent can use tools like `qml_find_and_click`, `qml_screenshot`, `qml_list_interactive`, etc. See the [logos-qt-mcp README](https://github.com/logos-co/logos-qt-mcp#readme) for the full list of available tools.

## Requirements

### Build Tools
- CMake (3.16 or later)
- Ninja build system
- pkg-config

### Dependencies
- Qt6 (qtbase)
- Qt6 Widgets (included in qtbase)
- Qt6 Remote Objects (qtremoteobjects)
- logos-liblogos
- logos-cpp-sdk (for header generation)
- logos-capability-module
- logos-package-manager
- zstd
- krb5
- abseil-cpp

## Disclaimer
This repository forms part of an experimental development environment and is not intended for production use.

See the Logos Core repository for additional information about the experimental development environment: https://github.com/logos-co/logos-liblogos
