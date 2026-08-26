#!/usr/bin/env node
// ---------------------------------------------------------------------------
// logos-basecamp host-services grant guard
//
// One assertion, deliberately: a non-"core" identity must actually complete a
// capability-gated call chain. See tests/host-services-assert.mjs for the full
// derivation of why `pmui.BackendStore.repositoryCount` is the right witness
// and why the neighbouring numbers are not.
//
// Usage:
//   node tests/host-services-tests.mjs --ci <app-binary> --verbose
//
// The nix wrapper (nix/host-services-test.nix) runs this with --verbose so the
// app's stderr is captured, then applies a SECOND, independent gate over that
// log: zero "rejecting unauthorized call" and zero "was not granted the
// <service> host service". That one is an absence assertion and could pass
// vacuously on its own (no calls made => no refusals), which is why the
// positive outcome assertion below runs first and in the same process.
// ---------------------------------------------------------------------------

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(__dirname, "..");
const qtMcpRoot = process.env.LOGOS_QT_MCP || resolve(projectRoot, "result-mcp");
const { test, run } = await import(resolve(qtMcpRoot, "test-framework/framework.mjs"));

const { assertHostServicesGrantReached } = await import(
  resolve(__dirname, "host-services-assert.mjs")
);

test("host-services: a non-core identity completes a capability-gated call chain", async (app) => {
  // 90s, not the default 10s: opening the section lazy-loads PMUI, which spawns
  // a ui-host process and waits on its ready handshake (PluginLoader allows 30s)
  // before any module call is even attempted.
  const store = await assertHostServicesGrantReached(app, {
    timeout: 90000,
    log: console.log,
  });

  // Second-order sanity: we must be reading the store of the identity whose
  // calls are actually gated. If PMUI is ever re-parented onto "core" this
  // whole guard silently stops testing anything, so fail loudly instead.
  if (store.moduleName !== "package_manager_ui") {
    throw new Error(
      `pmui.BackendStore.moduleName is ${JSON.stringify(store.moduleName)}, expected ` +
      `"package_manager_ui". This guard depends on PMUI's backend running under a ` +
      `NON-"core" identity — "core" holds ambient tokens and never goes through ` +
      `capability_module, which would make repositoryCount insensitive to the grant.`
    );
  }
});

run();
