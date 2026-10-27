// Standalone mode end to end with a backend that runs as a core module
// ("backend": "module"): the backend gets a module's context, the view reaches
// it but not the core, another app cannot reach it, and closing the app
// unloads it. Run by `nix build .#standalone-module-backend-test` through
// nix/mkPluginTest.nix.
import { readdirSync, statSync } from "node:fs";
import { join, resolve } from "node:path";

const root = process.env.LOGOS_QT_MCP || new URL("../result-mcp", import.meta.url).pathname;
const { test, run } = await import(resolve(root, "test-framework/framework.mjs"));

const userDir = process.env.BASECAMP_TEST_USER_DIR;

// The backend writes these into its module data folder (the fixture's
// onContextReady / aboutToUnload). Only a core module gets that folder.
function findMarker(name) {
  const walk = (dir) => {
    let entries = [];
    try { entries = readdirSync(dir); } catch { return null; }
    for (const e of entries) {
      const p = join(dir, e);
      if (e === name) return p;
      if (statSync(p).isDirectory()) {
        const hit = walk(p);
        if (hit) return hit;
      }
    }
    return null;
  };
  return userDir ? walk(join(userDir, "module_data")) : null;
}

// Waits for a string property on a fixture root to leave "pending".
async function waitForAnswer(app, objectId, property, description, timeout = 30000) {
  let value = "pending";
  await app.waitFor(async () => {
    value = await evalOn(app, objectId, property);
    if (value === "pending") throw new Error(`${property} not answered yet`);
  }, { timeout, interval: 500, description });
  return value;
}

async function requireObject(app, objectName) {
  let match = null;
  await app.waitFor(async () => {
    match = ((await app.findByProperty("objectName", objectName)).matches ?? [])[0];
    if (!match) throw new Error(`no ${objectName}`);
  }, { timeout: 30000, interval: 500, description: `${objectName} to exist` });
  return match;
}

async function evalOn(app, objectId, expression) {
  const res = await app.inspector.send("evaluate", { objectId, expression });
  if (res.error) throw new Error(`evaluate("${expression}") failed: ${res.error}`);
  return res.result;
}

test("module backend: a slot call round-trips", async (app) => {
  await app.waitFor(
    async () => { await app.expectTexts(["Backend: connected", "Backend sum: 5"]); },
    { timeout: 60000, interval: 500, description: "add(2, 3) to come back from the backend module" }
  );
});

test("module backend: it runs as a core module, with a data folder", async () => {
  if (!findMarker("backend-loaded"))
    throw new Error("no backend-loaded marker under module_data: the backend did not get a module context");
});

test("module backend: the view has no core access", async (app) => {
  const view = await requireObject(app, "fixture.root");
  const access = await waitForAnswer(app, view.id, "coreAccess", "the view's core call to fail");
  if (access !== "none") throw new Error(`the view reached the core: ${access}`);
});

test("module backend: another app cannot reach it", async (app) => {
  const intruder = await requireObject(app, "intruder.root");
  await evalOn(app, intruder.id, "attack()");
  const control = await waitForAnswer(app, intruder.id, "control", "the intruder's control call");
  if (!control.startsWith("REACHED"))
    throw new Error(`the intruder's control call failed, so its refusals prove nothing: ${control}`);
  for (const attempt of ["endpoint", "action", "replica"]) {
    const outcome = await waitForAnswer(app, intruder.id, attempt, `the intruder's ${attempt} attempt`);
    if (outcome.startsWith("REACHED")) throw new Error(`the intruder got through (${attempt}): ${outcome}`);
  }
});

test("module backend: closing the app unloads it", async (app) => {
  const workspace = await requireObject(app, "workspace");
  await evalOn(app, workspace.id, `closeDock("module_backend_fixture")`);
  await app.waitFor(async () => {
    if (!findMarker("backend-unloaded")) throw new Error("no backend-unloaded marker yet");
  }, { timeout: 20000, interval: 500, description: "the backend's aboutToUnload to run" });
});

run();
