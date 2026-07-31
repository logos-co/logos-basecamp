#!/usr/bin/env node
// ---------------------------------------------------------------------------
// logos-basecamp UI integration tests
//
// Usage:
//   node tests/ui-tests.mjs                       # run all (app must be running)
//   node tests/ui-tests.mjs modules               # run tests matching "modules"
//   node tests/ui-tests.mjs --ci <app-binary>     # CI mode: launch app, test, kill
//
// Set LOGOS_QT_MCP to override the framework path (nix builds set this automatically).
// Default: ./result-mcp (built via: nix build .#logos-qt-mcp -o result-mcp)
// ---------------------------------------------------------------------------

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(__dirname, "..");
const qtMcpRoot = process.env.LOGOS_QT_MCP || resolve(projectRoot, "result-mcp");
const { test, run } = await import(resolve(qtMcpRoot, "test-framework/framework.mjs"));

// Helper: click a plugin's sidebar icon and wait for its UI to load.
// Plugins load asynchronously after clicking, so we wait for expected
// content to appear before proceeding.
async function openPlugin(app, name, expectedTexts, opts = {}) {
  await app.click(name);
  await app.waitFor(
    async () => { await app.expectTexts(expectedTexts); },
    { timeout: 10000, interval: 500, description: `"${name}" UI to load` }
  );
}

// --- Package Manager ---
//
// PMUI is no longer launched from the sidebar app launcher (filtered out
// in UIPluginManager::launcherApps); it now lives behind the dedicated
// "Package Manager" sidebar section button, which lazy-loads PMUI into
// MainContainer's QStackedWidget slot 2 on first click.
test("package_manager_ui: open and verify categories", async (app) => {
  // Offscreen CI: logos-qt-mcp findByProperty sees "Reload" but not the Install label
  // (Row contentItem). Assert Reload only; the UI itself is unchanged.
  await openPlugin(app, "Package Manager", ["Reload"]);
});

test("settings: shows Dashboard, Apps Inspector, Module Inspector entries", async (app) => {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Apps Inspector", "Module Inspector"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
});

test("settings: clicking Dashboard renders the Dashboard view", async (app) => {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Apps Inspector", "Module Inspector"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
  await app.click("Dashboard", { type: "LogosItemDelegate" });
  await app.waitFor(
    async () => { await app.expectTexts(["Commits"]); },
    { timeout: 10000, interval: 500, description: "Dashboard view to render" }
  );
});

// --- Inspectors (Apps + Module) ---
//
// Regression test: navigating to Module Inspector must show auto-loaded core
// modules (package_manager, capability_module) with a "Loaded" status badge,
// not "Not loaded". The bug we hit was that
// MainUIBackend::refreshCoreModules() called logos_core_refresh_modules(),
// which re-ran ModuleRegistry::discoverInstalledModules() and wiped the
// `loaded` flag of every module via `m_modules.insert(qName, freshInfo)`.
// The whole list then rendered as Not loaded with no CPU/Mem stats.
//
// The old "Settings → Modules" sub-tab (with UI Modules + Core Modules
// nested tabs) was split into two top-level Settings sections:
// "Apps Inspector" (UI plugins) and "Module Inspector" (core modules,
// with live CPU/memory + Interface drilldown).
async function openAppsInspector(app) {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Apps Inspector", "Module Inspector"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
  await app.click("Apps Inspector", { type: "LogosItemDelegate" });
  await app.waitFor(
    async () => { await app.expectTexts(["UI plugins available in this installation."]); },
    { timeout: 10000, interval: 500, description: "Apps Inspector to become active" }
  );
}

async function openModuleInspector(app) {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Apps Inspector", "Module Inspector"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
  await app.click("Module Inspector", { type: "LogosItemDelegate" });
  await app.waitFor(
    async () => { await app.expectTexts(["Core modules known to the runtime, with live resource usage."]); },
    { timeout: 10000, interval: 500, description: "Module Inspector to become active" }
  );
}

test("apps inspector: shows installed UI plugins", async (app) => {
  await openAppsInspector(app);
  await app.waitFor(
    async () => { await app.expectTexts(["Main UI", "Package Manager"]); },
    { timeout: 10000, interval: 500, description: "Apps Inspector list to populate" }
  );
});

test("module inspector: auto-loaded modules show as Loaded with Unload action", async (app) => {
  await openModuleInspector(app);

  // Wait for the module list to populate.
  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager"]); },
    { timeout: 10000, interval: 500, description: "Module Inspector list to populate" }
  );

  // ModuleStatusBadge renders "Loaded" for loaded modules and "Not loaded"
  // for unloaded ones (no parens). ModuleRowActions renders "Unload" (or
  // "Load"). With the refreshCoreModules bug, every module showed
  // "Not loaded" and only "Load" buttons appeared, so neither "Loaded" nor
  // "Unload" appeared anywhere in the Module Inspector table.
  await app.waitFor(
    async () => { await app.expectTexts(["Loaded", "Unload"]); },
    { timeout: 10000, interval: 500, description: "loaded status and Unload button to appear" }
  );
});

test("module inspector: loaded modules render CPU and memory stats", async (app) => {
  await openModuleInspector(app);

  // Wait for at least one loaded plugin to appear.
  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager", "Loaded"]); },
    { timeout: 10000, interval: 500, description: "loaded plugins to appear" }
  );

  // Live stats update every 2s. The current Module Inspector cell format
  // is "<num>%" for CPU and "<num> MB" for memory (the "CPU:" / "Mem:"
  // prefixes moved to the column headers). Verify the two column headers
  // are present AND that at least one numeric-with-unit value has rendered
  // — proof that the stats poll is actually populating rows.
  await app.waitFor(
    async () => { await app.expectTexts(["CPU", "Memory"]); },
    { timeout: 15000, interval: 500, description: "CPU and Memory column headers to render" }
  );
  await app.waitFor(
    async () => {
      const tree = await app.getTree({ depth: 40 });
      const treeStr = JSON.stringify(tree);
      // Match the actual delegate output: <digit>% and <digit> MB.
      if (!/\d\.\d%/.test(treeStr)) {
        throw new Error("No CPU percentage rendered for loaded modules");
      }
      if (!/\d\.\d MB/.test(treeStr)) {
        throw new Error("No memory-in-MB rendered for loaded modules");
      }
    },
    { timeout: 15000, interval: 2000, description: "CPU % and Memory MB values to appear" }
  );
});

test("module inspector: leaving and returning preserves loaded state", async (app) => {
  // Navigate to Settings → Module Inspector and wait for loaded modules.
  await openModuleInspector(app);

  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager", "Loaded"]); },
    { timeout: 10000, interval: 500, description: "Module Inspector to show loaded modules" }
  );

  // Navigate away to a different top-level section (Applications).
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Install and manage applications."]); },
    { timeout: 10000, interval: 500, description: "Applications view to render" }
  );

  // Navigate back to Settings → Module Inspector.
  await openModuleInspector(app);

  // The previously-loaded modules must still show as "Loaded" with the Unload action.
  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager", "Loaded", "Unload"]); },
    { timeout: 10000, interval: 500, description: "loaded state to be preserved after returning" }
  );
});

// --- Sidebar: sequential section opening ---
//
// Regression guard: opening multiple sidebar sections one after another
// must not crash, hang, or leave the sidebar in an inconsistent state.
// Each section is opened via its sidebar button, we wait for expected
// content to render, then move on to the next. Finally we verify each
// section is still reachable by switching back to it.
//
// (Previously this iterated launcher-installed plugins, but PMUI is now
// the only one and it lives behind a section button rather than the
// launcher, so this is now a section walk.)
test("sidebar: open multiple sections sequentially without failure", async (app) => {
  const sections = [
    { name: "Applications",    expect: ["Install and manage applications."] },
    { name: "Package Manager", expect: ["Reload"] },
    { name: "Settings",        expect: ["Manage modules, apps and dashboards.", "Sections"] },
  ];

  for (const section of sections) {
    await openPlugin(app, section.name, section.expect);
  }

  for (const section of sections) {
    await app.click(section.name);
    await app.waitFor(
      async () => { await app.expectTexts(section.expect); },
      { timeout: 10000, interval: 500, description: `"${section.name}" still accessible` }
    );
  }
});

// --- App Manager ---
test("app manager: panel + categories sidebar render on first open", async (app) => {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Apps", "Categories", "All"]); },
    { timeout: 15000, interval: 500, description: "App Manager content" }
  );
});

// ---------------------------------------------------------------------------
// Repositories view — disable vs remove semantics
// ---------------------------------------------------------------------------
//
// Regression guards for two related bugs:
//   (a) toggling the default off silently REMOVED it from the list,
//       indistinguishable from a real deletion — disable and remove
//       collapsed into a single state at the library layer.
//   (b) no coverage for the correct semantics: disable keeps the row with
//       enabled=false, remove drops it entirely, re-add restores it.
//
// The library now has independent defaultDisabled / defaultRemoved flags;
// these tests pin the observable behavior end-to-end through the coordinator.
async function openRepositoriesView(app) {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Sections", "Package Repositories"]); },
    { timeout: 10000, interval: 500, description: "Settings sections" }
  );
  await app.click("Package Repositories", { exact: true });
  await app.waitFor(
    async () => { await app.expectTexts(["Add a repository", "Default"]); },
    { timeout: 10000, interval: 500, description: "Repositories view content" }
  );
  const anchor = await app.findByProperty("text", "Package Repositories");
  if (!anchor.matches || anchor.matches.length === 0) {
    throw new Error("Package Repositories heading not found");
  }
  return anchor.matches[0].id;
}

async function isDefaultInList(app, anchorId) {
  return (await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) if (rs[i].isDefault) return true;
      return false;
    })()`,
  })).result === true;
}

async function isDefaultEnabled(app, anchorId) {
  return (await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) if (rs[i].isDefault) return rs[i].enabled !== false;
      return null;
    })()`,
  })).result;
}

test("repositories: disabling default keeps it in the list", async (app) => {
  const anchorId = await openRepositoriesView(app);

  const initialCount = (await app.inspector.send("evaluate", {
    objectId: anchorId, expression: "backend.repositories.length",
  })).result;
  if (typeof initialCount !== "number" || initialCount < 1) {
    throw new Error(`backend.repositories.length=${initialCount} (expected ≥ 1)`);
  }

  // Bypass the LogosSwitch click path (coordinate hit-testing on offscreen
  // is fragile). The bug lived in the library round-trip
  // (setRepositoryEnabled → getRepositories), which is exactly what this
  // flow exercises via `backend`.
  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) {
        if (rs[i].isDefault) { backend.setRepositoryEnabled(rs[i].url, false); return; }
      }
    })()`,
  });

  await app.waitFor(async () => {
    const count = (await app.inspector.send("evaluate", {
      objectId: anchorId, expression: "backend.repositories.length",
    })).result;
    if (count !== initialCount) {
      throw new Error(
        `default repo dropped from list after disable: length=${count}, ` +
        `initial=${initialCount}. This is the bug: disable must keep the row.`);
    }
    if ((await isDefaultEnabled(app, anchorId)) !== false) {
      throw new Error("default still shows enabled=true after disable");
    }
  }, { timeout: 5000, interval: 250, description: "disabled default to stay in list" });

  // Restore so subsequent tests start clean.
  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) {
        if (rs[i].isDefault) { backend.setRepositoryEnabled(rs[i].url, true); return; }
      }
    })()`,
  });
  await app.waitFor(async () => {
    if ((await isDefaultEnabled(app, anchorId)) !== true) {
      throw new Error("default did not re-enable");
    }
  }, { timeout: 5000, interval: 250, description: "re-enable to settle" });
});

// Full round trip: remove → re-add → assert restored.
//
// Re-add of a defaultRemoved URL goes through RepositoryRegistry::addRepository,
// which HTTPs-fetches the default's logos-repo.json before flipping the flag
// (downloader package_downloader_lib.cpp:485-514). Sandboxed nix builds /
// offscreen CI have no network, so this test is skipped there — the remove-only
// test below still covers the remove half.
//
// Runs FIRST in the pair (before the remove-only test) so that on local the
// state is restored between them: this test ends with the default present,
// then the remove-only test cleanly removes it. On offscreen this one skips,
// and the remove-only test runs against the initial default-present state.
test("repositories: removing default drops it from list; re-adding restores it", async (app) => {
  const anchorId = await openRepositoriesView(app);

  if (!(await isDefaultInList(app, anchorId))) {
    throw new Error("default not present at test start");
  }
  const defaultUrl = (await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) if (rs[i].isDefault) return rs[i].url;
      return "";
    })()`,
  })).result;
  if (typeof defaultUrl !== "string" || defaultUrl.length === 0) {
    throw new Error("could not read default url");
  }

  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `backend.removeRepository(${JSON.stringify(defaultUrl)})`,
  });
  await app.waitFor(async () => {
    if (await isDefaultInList(app, anchorId)) {
      throw new Error("default still in list after remove — remove must drop the row");
    }
  }, { timeout: 5000, interval: 250, description: "default row to disappear" });

  // Re-add by URL — flips both defaultRemoved and defaultDisabled off.
  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `backend.addRepository(${JSON.stringify(defaultUrl)})`,
  });
  await app.waitFor(async () => {
    if (!(await isDefaultInList(app, anchorId))) {
      throw new Error("default did not come back after addRepository");
    }
    if ((await isDefaultEnabled(app, anchorId)) !== true) {
      throw new Error("re-added default is not enabled");
    }
  }, { timeout: 5000, interval: 250, description: "default row to return enabled" });
}, { skip: ["offscreen"] });

// Remove is a pure flag flip on the downloader side (defaultRemoved=true),
// no network — safe to assert everywhere including offscreen CI. Runs
// AFTER the full round-trip test above so state ordering works on local:
//   local:      full test (ends default-present) → this test (removes) → done
//   offscreen:  full test skipped                → this test (removes) → done
test("repositories: removing default drops it from list", async (app) => {
  const anchorId = await openRepositoriesView(app);

  if (!(await isDefaultInList(app, anchorId))) {
    throw new Error("default not present at test start");
  }
  const defaultUrl = (await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) if (rs[i].isDefault) return rs[i].url;
      return "";
    })()`,
  })).result;
  if (typeof defaultUrl !== "string" || defaultUrl.length === 0) {
    throw new Error("could not read default url");
  }

  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `backend.removeRepository(${JSON.stringify(defaultUrl)})`,
  });
  await app.waitFor(async () => {
    if (await isDefaultInList(app, anchorId)) {
      throw new Error("default still in list after remove — remove must drop the row");
    }
  }, { timeout: 5000, interval: 250, description: "default row to disappear" });
});

// ---------------------------------------------------------------------------
// ShortcutBridge end-to-end
// ---------------------------------------------------------------------------
test("shortcut bridge: ⌘K in AppManager focuses the search bar", async (app) => {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Apps", "Categories"]); },
    { timeout: 15000, interval: 500, description: "App Manager visible" }
  );

  // QML declares sequence: "Ctrl+K". QShortcut.key stringifies as NativeText
  // — "⌘K" on macOS, "Ctrl+K" elsewhere — so try both.
  const keyForms = ["Ctrl+K", "⌘K"];
  let mirrors = [];
  for (const value of keyForms) {
    const mirrorSearch = await app.inspector.send("findByProperty", {
      property: "key", value,
    });
    mirrors = (mirrorSearch.matches ?? []).filter(
      m => (m.type ?? "").startsWith("QShortcut")
    );
    if (mirrors.length > 0) break;
  }
  if (mirrors.length === 0) {
    throw new Error(
      `ShortcutBridge did not mirror Ctrl+K on the host (tried ${keyForms.join(", ")})`
    );
  }

  const activated = await app.inspector.send("callMethod", {
    objectId: mirrors[0].id, method: "activated",
  });
  if (activated.error) {
    throw new Error(`callMethod(activated) failed: ${activated.error}`);
  }

  await app.waitFor(async () => {
    const bar = await app.inspector.send("findByProperty", {
      property: "placeholderText", value: "Search apps…",
    });
    const bid = bar.matches?.[0]?.id;
    if (!bid) throw new Error("Search apps… bar not found");
    const evalR = await app.inspector.send("evaluate", {
      objectId: bid, expression: "textInput.activeFocus",
    });
    if (evalR.result !== true)
      throw new Error(`textInput.activeFocus = ${evalR.result}, expected true`);
  }, { timeout: 3000, interval: 200, description: "search bar to focus" });
});

// --- App Manager "Local" section ---
//
// Two invariants for the synthetic "Local" repo bucket in AppManagerView:
//   (a) matchLocalOnly on AppsFilterProxy picks up exactly the rows in
//       AppsModel that have an empty repositoryUrl;
//   (b) the "Local" section header renders in the grid iff local rows exist.
//
// The test inspects the live QML tree — no fixture-seeding, so it verifies
// the invariant against whatever the harness has installed. If the harness
// has zero local packages the test short-circuits ("ok, no local rows"),
// which is fine: what we're guarding against is a wrong / silent-broken
// mapping, not fixture presence.
//
// Anchor: AppManagerView declares `AppsFilterProxy { objectName:
// "appManager.localAppsProxy"; matchLocalOnly: true; sourceModel:
// root.appsProxy }`. From that anchor we can reach both the proxy's own
// visibleCount and the underlying AppsModel via sourceModel.sourceModel.

async function findLocalAppsProxy(app) {
  const res = await app.findByProperty("objectName", "appManager.localAppsProxy");
  if (!res.matches || res.matches.length === 0) return null;
  return res.matches[0].id;
}

// Evaluate expects a primitive result — object literals come back as
// opaque "<QJSValue>". So we do one call per property. Small wrapper
// keeps call sites readable.
async function evalOn(app, objectId, expression) {
  const res = await app.inspector.send("evaluate", { objectId, expression });
  if (res.error) throw new Error(`evaluate("${expression}") failed: ${res.error}`);
  return res.result;
}

test("app manager: localAppsProxy is wired as a matchLocalOnly filter", async (app) => {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Apps", "Categories"]); },
    { timeout: 15000, interval: 500, description: "App Manager to render" }
  );

  const proxyId = await findLocalAppsProxy(app);
  if (proxyId === null) {
    throw new Error("appManager.localAppsProxy not found in QML tree");
  }

  // Guards the wiring: without matchLocalOnly=true the section would
  // silently absorb every catalog row when repositoryUrlFilter is empty.
  // Without a sourceModel it would report 0 forever.
  const matchLocalOnly = await evalOn(app, proxyId, "matchLocalOnly");
  if (matchLocalOnly !== true) {
    throw new Error(`matchLocalOnly=${matchLocalOnly} (expected true)`);
  }
  const excludeMainUi = await evalOn(app, proxyId, "excludeMainUi");
  if (excludeMainUi !== false) {
    throw new Error(`excludeMainUi=${excludeMainUi} (expected false — Local shows all)`);
  }
  const hasSourceModel = await evalOn(app, proxyId, "!!sourceModel");
  if (!hasSourceModel) {
    throw new Error("localAppsProxy.sourceModel is unset");
  }
  const visibleCount = await evalOn(app, proxyId, "visibleCount");
  if (typeof visibleCount !== "number" || visibleCount < 0) {
    throw new Error(`visibleCount=${visibleCount} (expected non-negative number)`);
  }
  const outerRowCount = await evalOn(app, proxyId, "sourceModel.rowCount()");
  if (visibleCount > outerRowCount) {
    throw new Error(
      `visibleCount=${visibleCount} exceeds outer proxy rowCount=${outerRowCount} — ` +
      `matchLocalOnly cannot legally count MORE rows than its source`);
  }
});

test("app manager: 'local' header renders iff local rows exist", async (app) => {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Apps", "Categories"]); },
    { timeout: 15000, interval: 500, description: "App Manager to render" }
  );

  const proxyId = await findLocalAppsProxy(app);
  if (proxyId === null) throw new Error("appManager.localAppsProxy not found");

  const vcRes = await app.inspector.send("evaluate", {
    objectId: proxyId, expression: "visibleCount",
  });
  if (vcRes.error) throw new Error(`evaluate(visibleCount) failed: ${vcRes.error}`);
  const localCount = typeof vcRes.result === "number" ? vcRes.result : 0;

  // Section header label is lowercase "local" — distinguishes the
  // synthetic bucket from publisher-authored repo display names.
  const headerHits = await app.inspector.send("findByProperty", {
    property: "text", value: "local",
  });
  const visibleLocalHeaders = [];
  for (const m of (headerHits.matches ?? [])) {
    try {
      const props = await app.inspector.send("getProperties", { objectId: m.id });
      const visibleProp = props.properties?.find(p => p.name === "visible");
      if (visibleProp && visibleProp.value === true) visibleLocalHeaders.push(m.id);
    } catch { /* ignore per-match failures */ }
  }

  if (localCount > 0 && visibleLocalHeaders.length === 0) {
    throw new Error(
      `localAppsProxy.visibleCount=${localCount} but no visible "local" ` +
      `header was found in the AppManager tree`);
  }
  // Absence is fine when localCount === 0 — the section's `visible` binds
  // on localFilter.visibleCount > 0 and correctly hides.
});

// --- Run ---

run();
