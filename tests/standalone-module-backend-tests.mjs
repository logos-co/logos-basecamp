// Standalone mode end to end with a backend that runs as a core module
// ("backend": "module"): the backend gets a module's context, the view reaches
// it but not the core, another app cannot reach it, a crashed backend gets
// Basecamp's stopped panel and restarts on Reopen, and closing the app
// unloads it. Run by `nix build .#standalone-module-backend-test` through
// nix/mkPluginTest.nix.
import { readdirSync, readFileSync, statSync, unlinkSync } from "node:fs";
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

test("module backend: it is closed to other apps before it starts", async () => {
  // capability_module lets anyone call a module it has no rule for, so the
  // rule must be in place before the core starts the backend. The session log
  // has Basecamp's and the core's lines in one stream, in order.
  const logs = join(userDir, "logs");
  const file = readdirSync(logs).filter((f) => /^basecamp_.*\.log$/.test(f)).sort().pop();
  if (!file) throw new Error(`no session log in ${logs}`);
  const log = readFileSync(join(logs, file), "utf8");
  const restricted = log.indexOf("Restricted module_backend_fixture_backend to module_backend_fixture");
  const loaded = log.indexOf("Module loaded: module_backend_fixture_backend");
  if (restricted < 0) throw new Error("no restriction line in the session log");
  if (loaded < 0) throw new Error("no load line for the backend in the session log");
  if (restricted > loaded) throw new Error("the backend started before it was restricted to its view");
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

// Every sidebar app has its own marker; true if any of them is showing.
async function anyStoppedBadgeVisible(app) {
  const matches = (await app.findByProperty("objectName", "sidebar.marker.stopped")).matches ?? [];
  for (const m of matches) {
    if (await evalOn(app, m.id, "visible").catch(() => false) === true) return true;
  }
  return false;
}

test("module backend: the Module Inspector leaves it out", async (app) => {
  // Part of the app: unloading it there would look like a crash.
  const anchor = await requireObject(app, "sidebar.app.module_backend_fixture");
  let names = [];
  await app.waitFor(async () => {
    names = JSON.parse(await evalOn(app, anchor.id, `(() => {
      const m = backend.coreModulesModel, out = [];
      for (let i = 0; i < m.rowCount(); i += 1) out.push(m.data(m.index(i, 0), Qt.UserRole + 1));
      return JSON.stringify(out);
    })()`));
    if (!names.includes("capability_module")) throw new Error(`module list not ready: ${names}`);
  }, { timeout: 30000, interval: 500, description: "the Module Inspector's list" });
  await app.expectTexts(["Backend: connected"]);
  if (names.includes("module_backend_fixture_backend"))
    throw new Error(`the app's backend is listed: ${JSON.stringify(names)}`);
  // hijack_by_name names the installed depsvc as its backend, and "shadow"
  // declares shadow_backend, an installed module's name: neither may hide it.
  for (const installed of ["depsvc", "shadow_backend"]) {
    if (!names.includes(installed))
      throw new Error(`an app hid the installed ${installed}: ${JSON.stringify(names)}`);
  }
});

// Which plugin file depsvc's host process has mapped. Linux only (/proc):
// the macOS build sandbox lets a test see no other process.
function depsvcMaps() {
  for (const entry of readdirSync("/proc")) {
    if (!/^\d+$/.test(entry)) continue;
    let maps = "";
    try { maps = readFileSync(`/proc/${entry}/maps`, "utf8"); } catch { continue; }
    if (maps.includes("depsvc_plugin") || maps.includes("hijack_by_plugin_backend_plugin")) return maps;
  }
  return null;
}

function expectRealDepsvc(maps, when) {
  const hijacked = maps.split("\n").find((l) => l.includes("/plugins/hijack_by_"));
  if (hijacked) throw new Error(`depsvc runs an app's plugin ${when}: ${hijacked}`);
  if (!maps.includes("/modules/depsvc/"))
    throw new Error(`depsvc's process does not map modules/depsvc ${when}`);
}

test("module backend: an app cannot take over an installed module's name", async (app) => {
  // depsvc is installed (modules/depsvc); both apps ship a copy of its plugin
  // as their "backend": one names depsvc outright, one names its own
  // <app>_backend but the plugin inside calls itself depsvc.
  const anchor = await requireObject(app, "sidebar.app.module_backend_fixture");
  const evalBackend = (expr) => evalOn(app, anchor.id, expr);
  const depsvcLoaded = async () => JSON.parse(await evalBackend(`(() => {
    const m = backend.coreModulesModel;
    for (let i = 0; i < m.rowCount(); i += 1)
      if (m.data(m.index(i, 0), Qt.UserRole + 1) === "depsvc")
        return JSON.stringify(m.data(m.index(i, 0), Qt.UserRole + 9) === true);
    return "false";
  })()`));
  const waitForDepsvc = (loaded, what) => app.waitFor(async () => {
    if (await depsvcLoaded() !== loaded) throw new Error(`depsvc not ${loaded ? "loaded" : "unloaded"} yet`);
  }, { timeout: 30000, interval: 500, description: what });
  const checkPlugin = async (when) => {
    if (process.platform !== "linux") return;  // see depsvcMaps
    let maps = null;
    await app.waitFor(async () => {
      maps = depsvcMaps();
      if (!maps) throw new Error("no depsvc process yet");
    }, { timeout: 30000, interval: 500, description: `depsvc's process ${when}` });
    expectRealDepsvc(maps, when);
  };

  await evalBackend(`backend.loadCoreModule("depsvc")`);
  await waitForDepsvc(true, "depsvc to load");
  await checkPlugin("before the attempts");

  for (const name of ["hijack_by_name", "hijack_by_plugin"]) {
    await evalBackend(`backend.loadUiModule(${JSON.stringify(name)})`);
    await app.waitFor(async () => {
      if (JSON.parse(await evalBackend("JSON.stringify(backend.loadingModules)")).includes(name))
        throw new Error(`${name} still loading`);
    }, { timeout: 30000, interval: 500, description: `${name}'s load to finish` });
    if ((await app.findByProperty("objectName", `${name}.root`)).matches?.length)
      throw new Error(`${name} opened: its backend was accepted`);
  }

  // A refused app must not have unloaded the module it named.
  await new Promise((r) => setTimeout(r, 2000));
  if (!(await depsvcLoaded())) throw new Error("depsvc was stopped by an app it does not belong to");

  // Next start of depsvc: from its own file, not an app's.
  await evalBackend(`backend.unloadCoreModule("depsvc")`);
  await waitForDepsvc(false, "depsvc to unload");
  await evalBackend(`backend.loadCoreModule("depsvc")`);
  await waitForDepsvc(true, "depsvc to load again");
  await checkPlugin("after the attempts");
});

test("module backend: a crashed backend shows a stopped panel, and Reopen restarts it", async (app) => {
  const loaded = findMarker("backend-loaded");
  if (!loaded) throw new Error("no backend-loaded marker to start from");
  unlinkSync(loaded);

  await evalOn(app, (await requireObject(app, "fixture.root")).id, "crashBackend()");

  const title = await requireObject(app, "appStopped.title");
  const titleText = await evalOn(app, title.id, "text");
  if (titleText !== "Module Backend Fixture stopped unexpectedly")
    throw new Error(`unexpected stopped-panel title: ${titleText}`);
  if (!(await anyStoppedBadgeVisible(app)))
    throw new Error("no sidebar badge is shown for the stopped app");

  const reopen = await requireObject(app, "appStopped.reopen");
  await evalOn(app, reopen.id, "clicked()");

  await app.waitFor(async () => {
    if (!findMarker("backend-loaded")) throw new Error("no new backend-loaded marker yet");
  }, { timeout: 60000, interval: 500, description: "a fresh backend to start" });
  await app.waitFor(
    async () => { await app.expectTexts(["Backend: connected", "Backend sum: 5"]); },
    { timeout: 60000, interval: 500, description: "the reopened view to reach the new backend" });
  if ((await app.findByProperty("objectName", "appStopped.root")).matches?.length)
    throw new Error("the stopped panel is still there after Reopen");
  if (await anyStoppedBadgeVisible(app))
    throw new Error("the sidebar badge is still shown after Reopen");
});

test("module backend: a second crash after Reopen is shown too", async (app) => {
  await evalOn(app, (await requireObject(app, "fixture.root")).id, "crashBackend()");

  await requireObject(app, "appStopped.title");
  await app.waitFor(async () => {
    if (!(await anyStoppedBadgeVisible(app))) throw new Error("no sidebar badge");
  }, { timeout: 10000, interval: 500, description: "the sidebar badge after the second crash" });

  const reopen = await requireObject(app, "appStopped.reopen");
  await evalOn(app, reopen.id, "clicked()");
  await app.waitFor(
    async () => { await app.expectTexts(["Backend: connected", "Backend sum: 5"]); },
    { timeout: 60000, interval: 500, description: "the view to reach a third backend" });
});

test("module backend: closing the app unloads it", async (app) => {
  const workspace = await requireObject(app, "workspace");
  await evalOn(app, workspace.id, `closeDock("module_backend_fixture")`);
  await app.waitFor(async () => {
    if (!findMarker("backend-unloaded")) throw new Error("no backend-unloaded marker yet");
  }, { timeout: 20000, interval: 500, description: "the backend's aboutToUnload to run" });
});

run();
