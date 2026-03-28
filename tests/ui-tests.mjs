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

// --- Webview App ---
// Skipped in offscreen mode: QWebEngine requires a display (GPU/compositor)

test("webview_app: open and verify buttons", async (app) => {
  await app.click("webview_app");

  await app.expectTexts(["Wikipedia", "Local File", "Send Event to WebApp"]);
}, { skip: ["offscreen"] });

test("webview_app: click Wikipedia", async (app) => {
  await app.click("webview_app");
  await app.click("Wikipedia", { type: "QPushButton" });

  await app.expectTexts(["Wikipedia", "Local File"]);
}, { skip: ["offscreen"] });

test("webview_app: click Local File", async (app) => {
  await app.click("webview_app");
  await app.click("Local File", { type: "QPushButton" });

  await app.expectTexts(["Wikipedia", "Local File"]);
}, { skip: ["offscreen"] });

// --- Package Manager ---

test("package_manager_ui: open and verify categories", async (app) => {
  await app.click("package_manager_ui");

  await app.expectTexts(["Reload", "Install"]);
});

// --- Accounts (portable/distributed builds only) ---

test("accounts: install via package manager", async (app) => {
  if (!process.env.LOGOS_PORTABLE) {
    console.log("    (skipped — not a portable build)");
    return;
  }

  // Open package manager and filter to Accounts
  await app.click("package_manager_ui");
  await app.click("Accounts");

  // Select both accounts packages via checkboxes
  const res = await app.inspector.send("findByProperty", { property: "checkState", value: "Unchecked" });
  const ids = [...new Set(res.matches.map(m => m.id))];
  for (const id of ids) {
    await app.inspector.send("click", { objectId: id });
  }

  // Install selected packages
  await app.click("Install", { exact: true });

  // Wait for installation to complete
  await app.waitFor(async () => {
    const r = await app.inspector.send("findByProperty", { property: "text", value: "Installed" });
    return r.matches && r.matches.length >= 2;
  }, { timeout: 60000, interval: 1000, description: "accounts packages installed" });

  await app.expectTexts(["Installed"]);

  // Navigate to Modules view (sidebar button is icon-only, find by property)
  const modulesBtn = await app.inspector.send("findByProperty", { property: "text", value: "Modules" });
  const btn = modulesBtn.matches.find(m => m.type.includes("SidebarCircleButton"));
  await app.inspector.send("click", { objectId: btn.id });
  await new Promise(r => setTimeout(r, 2000));

  // Load accounts_ui from the UI Modules list
  await app.click("accounts_ui");
  await new Promise(r => setTimeout(r, 1000));
  // Click the Load button next to accounts_ui
  await app.click("Load");
  await new Promise(r => setTimeout(r, 5000));

  // Open accounts UI from sidebar
  await app.click("accounts_ui");
  await new Promise(r => setTimeout(r, 3000));

  // Generate a mnemonic
  await app.click("Create Random Mnemonic");
  await app.expectTexts(["Mnemonic created successfully"]);
}, { skip: ["normal"] });

// --- Counter ---

test("counter: open app", async (app) => {
  await app.click("counter");
});

test("counter: increment twice and expect value 2", async (app) => {
  await app.click("counter");

  // Verify counter starts at 0
  await app.expectTexts(["0"]);

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
