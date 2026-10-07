// Standalone mode end to end with a C++ backend: the app's backend starts in
// ui-host, its replica reaches the view, and a slot call round-trips. Run by
// `nix build .#standalone-backend-test` through nix/mkPluginTest.nix.
import { resolve } from "node:path";

const root = process.env.LOGOS_QT_MCP || new URL("../result-mcp", import.meta.url).pathname;
const { test, run } = await import(resolve(root, "test-framework/framework.mjs"));

test("standalone: the backend connects", async (app) => {
  await app.waitFor(
    async () => { await app.expectTexts(["Backend: connected"]); },
    { timeout: 60000, interval: 500, description: "the backend replica to become ready" }
  );
});

test("standalone: a backend slot call round-trips", async (app) => {
  await app.waitFor(
    async () => { await app.expectTexts(["Backend sum: 5"]); },
    { timeout: 30000, interval: 500, description: "add(2, 3) to come back from ui-host" }
  );
});

run();
