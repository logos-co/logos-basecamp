// Standalone mode end to end: Basecamp started with `--module <install root>`
// opens the app as a tab, resolves its dependencies and serves its bridge
// calls. Run by `nix build .#standalone-test` through nix/mkPluginTest.nix.
import { resolve } from "node:path";

const root = process.env.LOGOS_QT_MCP || new URL("../result-mcp", import.meta.url).pathname;
const { test, run } = await import(resolve(root, "test-framework/framework.mjs"));

test("standalone: --module app opens as a tab", async (app) => {
  await app.waitFor(
    async () => { await app.expectTexts(["Standalone fixture loaded"]); },
    { timeout: 30000, interval: 500, description: "the fixture view to load" }
  );
});

test("standalone: a bridge call reaches its dependency", async (app) => {
  await app.waitFor(
    async () => { await app.expectTexts(["Bridge call: ok"]); },
    { timeout: 30000, interval: 500, description: "package_manager to answer through the bridge" }
  );
});

test("standalone: the sidebar offers only Settings", async (app) => {
  // By objectName: ContentViews keeps every view instantiated, so the App
  // Manager's "Applications" heading is in the tree even when not offered.
  const count = async (name) =>
    ((await app.findByProperty("objectName", name)).matches ?? []).length;
  if (await count("sidebar.section.3") !== 1) throw new Error("Settings is not in the sidebar");
  for (const [section, label] of [[1, "Applications"], [2, "Package Manager"]]) {
    if (await count(`sidebar.section.${section}`) !== 0)
      throw new Error(`"${label}" is in the sidebar of a standalone host`);
  }
});

run();
