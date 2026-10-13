// Standalone hot reload end to end: Basecamp started with --qml-source loads
// the app's view from a source dir and rebuilds it when a file there changes.
// HOT_RELOAD_SRC is that dir, a writable copy of the fixture's source. Run by
// `nix build .#standalone-hot-reload-test` through nix/mkPluginTest.nix.
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const root = process.env.LOGOS_QT_MCP || new URL("../result-mcp", import.meta.url).pathname;
const { test, run } = await import(resolve(root, "test-framework/framework.mjs"));
const entry = resolve(process.env.HOT_RELOAD_SRC, "Main.qml");

test("hot reload: the view loads from the source dir", async (app) => {
  await app.waitFor(
    async () => { await app.expectTexts(["Standalone fixture loaded"]); },
    { timeout: 30000, interval: 500, description: "the fixture view to load" }
  );
});

test("hot reload: an edit is picked up without a restart", async (app) => {
  const original = readFileSync(entry, "utf8");
  writeFileSync(entry, original.replace("Standalone fixture loaded", "Standalone fixture reloaded"));
  await app.waitFor(
    async () => { await app.expectTexts(["Standalone fixture reloaded"]); },
    { timeout: 15000, interval: 300, description: "the edited view to replace the old one" }
  );
});

test("hot reload: a broken save recovers on the next good one", async (app) => {
  const good = readFileSync(entry, "utf8");
  writeFileSync(entry, good + "\n}\n");   // unbalanced: does not compile
  await new Promise((r) => setTimeout(r, 1500));
  writeFileSync(entry, good.replace("Standalone fixture reloaded", "Standalone fixture recovered"));
  await app.waitFor(
    async () => { await app.expectTexts(["Standalone fixture recovered"]); },
    { timeout: 15000, interval: 300, description: "the fixed view to come back" }
  );
});

run();
