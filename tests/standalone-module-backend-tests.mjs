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

// The backend's host process, found by the module name on its command line.
function backendPids() {
  const pids = [];
  for (const entry of readdirSync("/proc")) {
    if (!/^\d+$/.test(entry) || Number(entry) === process.pid) continue;
    let cmd = "";
    try { cmd = readFileSync(`/proc/${entry}/cmdline`, "utf8"); } catch { continue; }
    if (cmd.includes("module_backend_fixture_backend")) pids.push(Number(entry));
  }
  return pids;
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
  if (backendPids().length === 0) throw new Error("the backend is not running");
  if (names.includes("module_backend_fixture_backend"))
    throw new Error(`the app's backend is listed: ${JSON.stringify(names)}`);
  // hijack_by_name names the installed depsvc as its backend; that must not hide it.
  if (!names.includes("depsvc"))
    throw new Error(`an app hid an installed module: ${JSON.stringify(names)}`);
});

// The files a process has mapped: which plugin a module's host really runs.
function mappedPlugins(pid) {
  try { return readFileSync(`/proc/${pid}/maps`, "utf8"); } catch { return ""; }
}

// The process running a depsvc plugin, and the files it has mapped.
function depsvcProcess() {
  for (const entry of readdirSync("/proc")) {
    if (!/^\d+$/.test(entry)) continue;
    const maps = mappedPlugins(entry);
    if (maps.includes("depsvc_plugin") || maps.includes("hijack_by_plugin_backend_plugin"))
      return { pid: Number(entry), maps };
  }
  return null;
}

function expectRealDepsvc(proc, when) {
  const hijacked = proc.maps.split("\n").find((l) => l.includes("/plugins/hijack_by_"));
  if (hijacked) throw new Error(`depsvc runs an app's plugin ${when}: ${hijacked}`);
  if (!proc.maps.includes("/modules/depsvc/"))
    throw new Error(`depsvc's process does not map modules/depsvc ${when}`);
}

test("module backend: an app cannot take over an installed module's name", async (app) => {
  // depsvc is installed (modules/depsvc); both apps ship a copy of its plugin
  // as their "backend": one names depsvc outright, one names its own
  // <app>_backend but the plugin inside calls itself depsvc.
  const anchor = await requireObject(app, "sidebar.app.module_backend_fixture");
  const evalBackend = (expr) => evalOn(app, anchor.id, expr);
  const waitForDepsvc = async (what, notPid = 0) => {
    let proc = null;
    await app.waitFor(async () => {
      proc = depsvcProcess();
      if (!proc || proc.pid === notPid) throw new Error("no depsvc process yet");
    }, { timeout: 30000, interval: 500, description: what });
    return proc;
  };

  await evalBackend(`backend.loadCoreModule("depsvc")`);
  const before = await waitForDepsvc("depsvc to load");
  expectRealDepsvc(before, "before the attempts");

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
  const still = depsvcProcess();
  if (!still || still.pid !== before.pid)
    throw new Error("depsvc was stopped by an app it does not belong to");

  // Next start of depsvc: from its own file, not an app's.
  await evalBackend(`backend.unloadCoreModule("depsvc")`);
  await app.waitFor(async () => {
    if (depsvcProcess()?.pid === before.pid) throw new Error("depsvc still running");
  }, { timeout: 30000, interval: 500, description: "depsvc to unload" });
  await evalBackend(`backend.loadCoreModule("depsvc")`);
  expectRealDepsvc(await waitForDepsvc("depsvc to load again", before.pid), "after the attempts");
});

test("module backend: a crashed backend shows a stopped panel, and Reopen restarts it", async (app) => {
  const loaded = findMarker("backend-loaded");
  if (!loaded) throw new Error("no backend-loaded marker to start from");
  unlinkSync(loaded);

  const pids = backendPids();
  if (pids.length === 0) throw new Error("no process with module_backend_fixture_backend on its command line");
  for (const pid of pids) process.kill(pid, "SIGKILL");

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
  const pids = backendPids();
  if (pids.length === 0) throw new Error("no reopened backend to crash");
  for (const pid of pids) process.kill(pid, "SIGKILL");

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
