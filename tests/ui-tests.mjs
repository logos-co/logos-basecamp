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
  await openPlugin(app, "package_manager_ui", ["Reload", "Install"]);
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

  // Verify counter shows 2
  await app.expectTexts(["2"]);
});

// --- Counter QML ---

test("counter_qml: open app", async (app) => {
  await app.click("counter_qml");
});

// --- Run ---

run();
