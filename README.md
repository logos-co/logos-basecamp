# logos-basecamp

## Download

Prebuilt binaries are available for Linux (AppImage) and macOS (DMG) from the [latest release](https://github.com/logos-co/logos-basecamp/releases/latest).

## How to Build

### Using Nix (Recommended)

#### Local Build

The local build produces a standard Nix derivation whose dependencies live in `/nix/store`. It is the fastest way to iterate during development but is **not portable** — it only runs on the machine that built it.

```bash
nix build '.#app'
./result/bin/logos-basecamp
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
nix build '.#bin-bundle-dir'
./result/bin/LogosBasecamp
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
- `nix/default.nix` - Common configuration and main application build
- `nix/app.nix` - Application-specific compilation settings
- `nix/main-ui.nix` - UI components compilation

## Modules

### Blockchain

The *Blockchain App* lets you run your own blockchain node.

The app currently supports:
1. Joining the Logos Testnet
2. Participating in consensus (chain-following and proposing blocks)
3. Making and receiving transfers

You can run the Blockchain App through the Logos App, or standalone by building and running the app from source, instructions [here](https://github.com/logos-blockchain/logos-blockchain-ui?tab=readme-ov-file#how-to-build).

### LEZ Wallet

The *Logos Execution Zone Wallet App* lets you interact with the Logos Execution Zone (LEZ). It is currently limited to basic account operations. This showcases both private and public execution through RISCV emulation and ZK Proofs.

The wallet currently supports:
1. Initializing private/public accounts
2. Inspecting private/public account balances
3. Public to Public transfers
4. Private to Private transfers

You can run the LEZ Wallet through Logos App, or standalone by building and running from source, see instructions [here](https://github.com/logos-blockchain/logos-execution-zone-wallet-ui?tab=readme-ov-file#how-to-build).

### Storage

The Logos Storage App allows you to publish, download, and share files with other Logos users. You can run it both in standalone mode, or as part of the main Logos App.

Sharing files requires direct connection across nodes, so you will need to set up your router to allow NAT traversal either via UPnP, or manual port forwarding. The app will help you figure out if your NAT traversal is working with a reachability check.

Check [the app's README file](https://github.com/logos-co/logos-storage-ui) for more information on how to build, run, and troubleshoot it.

### Chat

The Logos Chat App lets you send and receive private 1:1 messages, where messages are transferred over Logos Delivery, the decentralised transport layer. You can run it both in standalone mode, or as part of the main Logos App.

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

You can run Mix Demo Chat inside the Logos App.
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
node tests/ui-tests.mjs --ci ./result/bin/logos-basecamp

# Or run against an already-running app:
node tests/ui-tests.mjs

# Run a subset:
node tests/ui-tests.mjs counter
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
