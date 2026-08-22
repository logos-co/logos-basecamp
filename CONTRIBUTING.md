# Contributing to Logos Basecamp

Thanks for wanting to contribute. This guide covers the mechanics — branch naming,
build/test flow, PR expectations — so your change lands cleanly.

## Before you start

- **Bug fixes**: open (or find) a GitHub issue first if the bug isn't obvious. Small typo/formatting PRs can skip this.
- **New features**: open an issue with the feature template first. We'd rather discuss shape before you invest implementation time.
- **Security issues**: do NOT open a public issue. Use [GitHub Private Vulnerability Reporting](https://github.com/logos-co/logos-basecamp/security/advisories/new).

## Branch naming

Prefix branches so the intent is legible at a glance and CI/label automation can key off them:

- `fix/<short-slug>` — bug fix
- `feat/<short-slug>` — new capability
- `chore/<short-slug>` — dependency bumps, housekeeping
- `docs/<short-slug>` — docs-only
- `test/<short-slug>` — test-only
- `ci/<short-slug>` — workflow / build-plumbing

Match the prefixes used on `master` — see `git log --oneline` for examples.

## Commit messages

- One purpose per commit. If your PR does two unrelated things, split it.
- Subject line: `<type>: <imperative summary>` (mirroring branch prefixes: `fix:`, `feat:`, `chore:`, etc.).
- Body: wrap at ~72 cols. Explain the *why*; the diff shows the *what*.
- Reference issues in the body (`Refs #123`, `Closes #456`).

## Building and testing

All builds go through Nix — do not run `cmake` directly.

```bash
nix build .#app                    # local (non-portable) dev build
./result/bin/LogosBasecamp

nix build .#smoke-test -L          # validates the app starts without QML errors
nix build .#integration-test -L    # end-to-end UI tests (hermetic, CI-safe)
nix build .#host-services-test -L  # capability trust root actually works (see below)
nix build .#doctests -L            # C++ doctests
```

`host-services-test` is the guard on the capability trust root: it asserts that a
NON-`core` identity (ui-host, running `package_manager_ui`) actually *completes* a
token-gated call chain, which is only possible when `capability_module` really
received its `token_registry` / `token_delivery` host-services grant from the
module loader this build pins. Run it after touching any of `logos-liblogos`,
`logos-module-loader-qt`, `logos-protocol`, `logos-plugin-qt` or
`logos-capability-module` pins — a loader that predates the grant makes
capability_module fail closed, and every other check stays green.

Portable release-shaped builds:

```bash
nix build .#bin-bundle-dir         # directory bundle (Linux + macOS)
nix build .#bin-appimage           # Linux AppImage
nix build .#bin-macos-app          # macOS .app bundle
```

### Iterating on QML without rebuilding

Point `DEV_QML_PATH` at the source tree — the three top-level view entries are read from disk. See [`CLAUDE.md`](./CLAUDE.md) for the covered entries and their limits.

## Before you open a PR

- [ ] Rebase (don't merge) `master` into your branch — keep history linear.
- [ ] `nix build .#smoke-test -L` passes locally.
- [ ] For UI changes, `nix build .#integration-test -L` passes and you've attached a screenshot/recording.
- [ ] No unrelated files (`.DS_Store`, editor temp files, generated `result` symlinks, screenshots).
- [ ] `README.md` / `CLAUDE.md` / `docs/` updated if behaviour or build steps changed.

## PR review flow

1. Open the PR against `master` using the template.
2. GitHub auto-assigns reviewers via [CODEOWNERS](./.github/CODEOWNERS).
3. CI (`build.yml`, `doctests.yml`) must pass — smoke + integration tests on Linux (x86 + ARM) and macOS.
4. At least one CODEOWNERS approval is required.
5. Squash-merge is the default; use merge-commit only for multi-commit PRs where each commit is meaningful on its own.

## Release cadence

Releases are cut from `release/**` branches by the maintainers — see [`docs/RELEASE.md`](./docs/RELEASE.md). If your fix needs to ship in the current release, mention that in the PR description and tag @Khushboo-dev-cpp.

## Style and conventions

- **C++**: C++17. No formatter enforced yet (`.clang-format` planned) — match the style of files you touch.
- **QML**: Feature-axis `qt_add_qml_module` modules under `app/Basecamp/<Feature>/`. Follow the pattern: view files under a feature emit signals; backend calls belong in `Basecamp/Shell/ContentViews.qml`. See `CLAUDE.md` for the C++ backend split (`MainUIBackend` / `CoreModuleManager` / `UIPluginManager` / `PackageCoordinator`) — respect the dependency direction.
- **Nix**: modular files under `nix/`. `flake.nix` inputs follow `logos-cpp-sdk`'s `nixpkgs` — never pin a separate one.

## Licensing

By contributing, you agree that your contribution is dual-licensed under [Apache 2.0](./LICENSE-APACHE-v2) and [MIT](./LICENSE-MIT), matching the repo license.
