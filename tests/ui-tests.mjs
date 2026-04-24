#!/usr/bin/env node
// ---------------------------------------------------------------------------
// logos-basecamp UI integration tests
//
// Usage:
//   node tests/ui-tests.mjs                       # run all (app must be running)
//   node tests/ui-tests.mjs counter               # run tests matching "counter"
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

// --- Webview App ---
// Skipped in offscreen mode: QWebEngine requires a display (GPU/compositor)

test("webview_app: open and verify buttons", async (app) => {
  await openPlugin(app, "webview_app", ["Wikipedia", "Local File", "Send Event to WebApp"]);
}, { skip: ["offscreen"] });

test("webview_app: click Wikipedia", async (app) => {
  await openPlugin(app, "webview_app", ["Wikipedia", "Local File"]);
  await app.click("Wikipedia", { type: "QPushButton" });

  await app.expectTexts(["Wikipedia", "Local File"]);
}, { skip: ["offscreen"] });

test("webview_app: click Local File", async (app) => {
  await openPlugin(app, "webview_app", ["Wikipedia", "Local File"]);
  await app.click("Local File", { type: "QPushButton" });

  await app.expectTexts(["Wikipedia", "Local File"]);
}, { skip: ["offscreen"] });

// --- Package Manager ---

test("package_manager_ui: open and verify categories", async (app) => {
  // Offscreen CI: logos-qt-mcp findByProperty sees "Reload" but not the Install label
  // (Row contentItem). Assert Reload only; the UI itself is unchanged.
  await openPlugin(app, "package_manager_ui", ["Reload"]);
});

// --- Counter ---

test("counter: open app", async (app) => {
  await app.click("counter");
});

test("counter: increment twice and expect value 2", async (app) => {
  await openPlugin(app, "counter", ["0"]);

  // Click increment twice
  await app.click("Increment me");
  await app.click("Increment me");

  // Verify counter shows 2. Use waitFor rather than a bare expectTexts —
  // Qt click → slot → widget repaint is asynchronous, and the click-to-
  // paint latency varies between Linux (QCocoa absent) and macOS (QCocoa
  // in the dispatch path even under -platform offscreen). On macOS we
  // were racing the paint and the assertion occasionally fired before
  // the "2" had landed in the rendered tree.
  await app.waitFor(
    async () => { await app.expectTexts(["2"]); },
    { timeout: 5000, interval: 200, description: "counter value to reach 2" }
  );
});

// --- Counter QML ---

test("counter_qml: open app", async (app) => {
  await app.click("counter_qml");
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
test("modules: core tab shows auto-loaded modules as Loaded", async (app) => {
  await app.click("Modules");
  await app.click("Core Modules");

  // Wait for the core modules list to populate.
  await app.waitFor(
    async () => { await app.expectTexts(["package_manager", "capability_module"]); },
    { timeout: 10000, interval: 500, description: "Core Modules list to populate" }
  );

  // CoreModulesView renders "(Loaded)" + "Unload Plugin" for loaded
  // modules and "(Not Loaded)" + "Load Plugin" for unloaded ones. With
  // the refreshCoreModules bug, every module showed "(Not Loaded)" and
  // the only buttons were "Load Plugin", so neither "(Loaded)" nor
  // "Unload Plugin" appeared anywhere in the UI.
  await app.expectTexts(["(Loaded)", "Unload Plugin"]);
});

test("modules: loaded plugins render CPU and memory stats", async (app) => {
  await app.click("Modules");
  await app.click("Core Modules");

  // Wait for at least one loaded plugin to appear.
  await app.waitFor(
    async () => { await app.expectTexts(["package_manager", "(Loaded)"]); },
    { timeout: 10000, interval: 500, description: "loaded plugins to appear" }
  );

  // CPU and memory stats update every 2s and values are dynamic
  // (e.g. "CPU: 0.0%", "Mem: 24.4 MB"). Use getTree with enough
  // depth to reach the deeply-nested LogosText elements and verify
  // the "CPU: " and "Mem: " prefixes appear in the rendered text.
  await app.waitFor(
    async () => {
      const tree = await app.getTree({ depth: 20 });
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
  // Navigate to Core Modules and wait for loaded plugins.
  await app.click("Modules");
  await app.click("Core Modules");

  await app.waitFor(
    async () => { await app.expectTexts(["package_manager", "(Loaded)"]); },
    { timeout: 10000, interval: 500, description: "Core Modules to show loaded plugins" }
  );

  // Navigate away to Dashboard.
  await app.click("Dashboard");
  await app.expectTexts(["Dashboard"]);

  // Navigate back to Modules > Core Modules.
  await app.click("Modules");
  await app.click("Core Modules");

  // The previously-loaded modules must still show as "(Loaded)" with stats.
  await app.waitFor(
    async () => { await app.expectTexts(["package_manager", "(Loaded)", "Unload Plugin"]); },
    { timeout: 10000, interval: 500, description: "loaded state to be preserved after returning" }
  );
});

// --- Sidebar: sequential plugin opening ---
//
// Regression guard: opening multiple plugins one after another must not
// crash, hang, or leave the sidebar in an inconsistent state. Each
// plugin is opened via its sidebar icon, we wait for its UI to load,
// then move on to the next. Finally we verify all opened plugins are
// still reachable by switching back to each one.

test("sidebar: open multiple plugins sequentially without failure", async (app) => {
  // Plugins available in the default fixture build (excluding webview_app
  // which requires a real display / GPU compositor).
  const plugins = [
    { name: "counter",              expect: ["0"] },
    { name: "counter_qml",         expect: ["0"] },
    { name: "package_manager_ui",  expect: ["Reload"] },
  ];

  // Open each plugin sequentially.
  for (const plugin of plugins) {
    await openPlugin(app, plugin.name, plugin.expect);
  }

  // Switch back to each plugin and verify its UI is still intact.
  // Clicking an already-loaded plugin in the sidebar should activate its
  // tab without reloading.
  for (const plugin of plugins) {
    await app.click(plugin.name);
    await app.waitFor(
      async () => { await app.expectTexts(plugin.expect); },
      { timeout: 10000, interval: 500, description: `"${plugin.name}" still accessible` }
    );
  }
});

// --- Run ---

run();
