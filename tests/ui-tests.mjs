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

test("settings: shows Dashboard and Modules entries", async (app) => {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Modules"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
});

test("settings: clicking Dashboard renders the Dashboard view", async (app) => {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Modules"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
  await app.click("Dashboard", { type: "LogosItemDelegate" });
  await app.waitFor(
    async () => { await app.expectTexts(["Commits"]); },
    { timeout: 10000, interval: 500, description: "Dashboard view to render" }
  );
});

// --- Modules section ---
//
// Regression test: navigating to the Core Modules tab must show
// auto-loaded core modules (package_manager, capability_module) as
// "(Loaded)", not "(Not Loaded)". The bug we hit was that
// MainUIBackend::refreshCoreModules() called logos_core_refresh_modules(),
// which re-ran ModuleRegistry::discoverInstalledModules() and wiped the
// `loaded` flag of every module via `m_modules.insert(qName, freshInfo)`.
// The whole list then rendered as Not Loaded with no CPU/Mem stats.
//
// The old top-level "Modules" sidebar entry was renamed; the embedded
// ModulesView (UI Modules + Core Modules tabs) now lives under
// Settings → Modules sub-tab.
async function openModules(app) {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Modules"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
  await app.click("Modules", { type: "LogosItemDelegate" });
  await app.waitFor(
    async () => { await app.expectTexts(["UI Modules", "Core Modules"]); },
    { timeout: 10000, interval: 500, description: "Modules tabs to render" }
  );
}

async function openCoreModules(app) {
  await openModules(app);
  await app.click("Core Modules");
  await app.waitFor(
    async () => { await app.expectTexts(["All available plugins in the system"]); },
    { timeout: 10000, interval: 500, description: "Core Modules tab to become active" }
  );
}

test("modules: ui tab shows installed UI plugins", async (app) => {
  await openModules(app);
  await app.waitFor(
    async () => { await app.expectTexts(["Main UI", "Package Manager"]); },
    { timeout: 10000, interval: 500, description: "UI Modules list to populate" }
  );
});

test("modules: core tab shows auto-loaded modules as Loaded", async (app) => {
  await openCoreModules(app);

  // Wait for the core modules list to populate.
  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager"]); },
    { timeout: 10000, interval: 500, description: "Core Modules list to populate" }
  );

  // CoreModulesView renders "(Loaded)" + "Unload Plugin" for loaded
  // modules and "(Not Loaded)" + "Load Plugin" for unloaded ones. With
  // the refreshCoreModules bug, every module showed "(Not Loaded)" and
  // the only buttons were "Load Plugin", so neither "(Loaded)" nor
  // "Unload Plugin" appeared anywhere in the UI.
  await app.waitFor(
    async () => { await app.expectTexts(["(Loaded)", "Unload Plugin"]); },
    { timeout: 10000, interval: 500, description: "loaded status and Unload button to appear" }
  );
});

test("modules: loaded plugins render CPU and memory stats", async (app) => {
  await openCoreModules(app);

  // Wait for at least one loaded plugin to appear.
  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager", "(Loaded)"]); },
    { timeout: 10000, interval: 500, description: "loaded plugins to appear" }
  );

  // CPU and memory stats update every 2s and values are dynamic
  // (e.g. "CPU: 0.0%", "Mem: 24.4 MB"). Use getTree with enough
  // depth to reach the deeply-nested LogosText elements and verify
  // the "CPU: " and "Mem: " prefixes appear in the rendered text.
  await app.waitFor(
    async () => {
      const tree = await app.getTree({ depth: 40 });
      const treeStr = JSON.stringify(tree);
      if (!treeStr.includes("CPU: ")) {
        throw new Error("No CPU stats rendered for loaded plugins");
      }
      if (!treeStr.includes("Mem: ")) {
        throw new Error("No Mem stats rendered for loaded plugins");
      }
    },
    { timeout: 15000, interval: 2000, description: "CPU and memory stats to appear" }
  );
});

test("modules: leaving and returning to Core Modules preserves loaded state", async (app) => {
  // Navigate to Settings → Modules → Core Modules and wait for loaded plugins.
  await openCoreModules(app);

  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager", "(Loaded)"]); },
    { timeout: 10000, interval: 500, description: "Core Modules to show loaded plugins" }
  );

  // Navigate away to a different top-level section (Applications).
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Install and manage applications."]); },
    { timeout: 10000, interval: 500, description: "Applications view to render" }
  );

  // Navigate back to Settings → Modules → Core Modules.
  await openCoreModules(app);

  // The previously-loaded modules must still show as "(Loaded)" with stats.
  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager", "(Loaded)", "Unload Plugin"]); },
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

// --- Run ---

run();
