#!/usr/bin/env node
// ---------------------------------------------------------------------------
// Basecamp's own apps run only from its bundle (app/utils/BuiltInApps.h), end
// to end with the real core and package manager. Run by
// `nix build .#builtin-apps-test` (nix/builtin-apps-test.nix) once per
// BUILTIN_SCENARIO, each with a fresh LOGOS_USER_DIR:
//
//   moved  Copies of Package Manager and App Manager planted before start are
//          moved aside and Basecamp's own open. Copies planted while it runs
//          move on the next rescan; the backend running under that name is
//          stopped, and Reopen brings back Basecamp's own.
//   stuck  Copies that cannot be moved (set-aside is a file) stay where they
//          are, and both apps are refused rather than opened from them.
//
// The copies are Basecamp's own apps claiming a newer version (the screen)
// and their backend plugins installed as core modules (the backend): the real
// thing, so only where they run from tells them apart. On Linux the
// files each process has mapped say that; the nix file's log gates check what
// Basecamp told the user, on every platform.
// ---------------------------------------------------------------------------

import { chmodSync, copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, readdirSync,
         realpathSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const qtMcpRoot = process.env.LOGOS_QT_MCP || resolve(__dirname, "../result-mcp");
const { test, run } = await import(resolve(qtMcpRoot, "test-framework/framework.mjs"));
const { PMUI_SIDEBAR_SECTION, readPmuiBackendStore } =
  await import(resolve(__dirname, "host-services-assert.mjs"));

const scenario = process.env.BUILTIN_SCENARIO;
const appRoot = process.env.BUILTIN_APP_ROOT;
const userDir = process.env.LOGOS_USER_DIR;
if (!["moved", "stuck"].includes(scenario) || !appRoot || !userDir)
  throw new Error("set BUILTIN_SCENARIO (moved or stuck), BUILTIN_APP_ROOT and LOGOS_USER_DIR");

// Each app as Basecamp bundles it, and where its planted copies go.
const [packageManager, appManager] = ["package_manager_ui", "app_manager_ui"].map((name) => {
  const bundled = join(appRoot, "plugins", name);
  const manifest = JSON.parse(readFileSync(join(bundled, "manifest.json"), "utf8"));
  const variant = readFileSync(join(bundled, "variant"), "utf8").trim();
  return { name, bundled, manifest, variant, backendFile: manifest.main[variant],
           screen: `plugins/${name}`, backend: `modules/${name}_backend` };
});
const setAside = join(userDir, "set-aside");
const copies = [packageManager, appManager].flatMap((a) => [a.screen, a.backend]);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The store's copies are read-only; a planted copy must not be.
function makeWritable(path) {
  chmodSync(path, statSync(path).isDirectory() ? 0o755 : 0o644);
  if (statSync(path).isDirectory())
    for (const entry of readdirSync(path)) makeWritable(join(path, entry));
}

function plant() {
  for (const { name, bundled, manifest, variant, backendFile, screen: screenDir, backend: backendDir }
       of [packageManager, appManager]) {
    const screen = join(userDir, screenDir);
    cpSync(bundled, screen, { recursive: true });
    makeWritable(screen);
    writeFileSync(join(screen, "manifest.json"), JSON.stringify({ ...manifest, version: "99.0.0" }));

    const backend = join(userDir, backendDir);
    mkdirSync(backend, { recursive: true });
    copyFileSync(join(bundled, backendFile), join(backend, backendFile));
    makeWritable(backend);
    writeFileSync(join(backend, "variant"), variant);
    writeFileSync(join(backend, "manifest.json"), JSON.stringify({
      name: `${name}_backend`, type: "core", version: "99.0.0", manifestVersion: "0.3.0",
      main: { [variant]: backendFile } }));
  }
}

const stillPlanted = () => copies.filter((copy) => existsSync(join(userDir, copy)));
const setAsideCount = () => (existsSync(setAside) && statSync(setAside).isDirectory()
  ? readdirSync(setAside).length : 0);

// Every file some process has mapped. Linux only: the macOS build sandbox
// shows a test no other process.
function mappedFiles() {
  const files = new Set();
  for (const pid of readdirSync("/proc").filter((e) => /^\d+$/.test(e))) {
    let maps = "";
    try { maps = readFileSync(`/proc/${pid}/maps`, "utf8"); } catch { continue; }
    for (const line of maps.split("\n")) {
      const at = line.indexOf("/");
      if (at >= 0) files.add(line.slice(at));
    }
  }
  return [...files];
}

const forms = (path) => { try { return [path, realpathSync(path)]; } catch { return [path]; } };
const under = (file, dirs) => dirs.some((dir) => forms(dir).some((d) => file.startsWith(d + "/")));

// Nothing planted, moved aside or not, runs.
function expectNoCopyRuns(when) {
  if (process.platform !== "linux") return;
  const dirs = ["plugins", "modules", "set-aside"].map((d) => join(userDir, d));
  const running = mappedFiles().filter((file) => under(file, dirs));
  if (running.length) throw new Error(`a planted copy runs ${when}: ${running.join(", ")}`);
}

// The backend that runs is Basecamp's own.
function expectBundledBackend(when, { backendFile } = packageManager) {
  if (process.platform !== "linux") return;
  const running = mappedFiles().filter((file) => file.endsWith("/" + backendFile));
  if (!running.length) throw new Error(`no process has ${backendFile} mapped ${when}`);
  const elsewhere = running.filter((file) => !under(file, [appRoot]));
  if (elsewhere.length) throw new Error(`the backend runs from ${elsewhere.join(", ")} ${when}`);
}

async function evalOn(app, objectId, expression) {
  const res = await app.inspector.send("evaluate", { objectId, expression });
  if (res.error) throw new Error(`evaluate("${expression}") failed: ${res.error}`);
  return res.result;
}

async function requireObject(app, objectName, timeout = 30000) {
  let match = null;
  await app.waitFor(async () => {
    match = ((await app.findByProperty("objectName", objectName)).matches ?? [])[0];
    if (!match) throw new Error(`no ${objectName}`);
  }, { timeout, interval: 500, description: `${objectName} to exist` });
  return match;
}

// Package Manager's store once it is up, or null if it never comes up.
async function packageManagerStore(app, timeout) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const store = await readPmuiBackendStore(app);
    if (store) return store;
    await sleep(1000);
  }
  return null;
}

async function waitUntilMoved(app, expected, description) {
  await app.waitFor(async () => {
    const left = stillPlanted();
    if (left.length) throw new Error(`still in the user's dirs: ${left.join(", ")}`);
  }, { timeout: 60000, interval: 500, description });
  if (setAsideCount() !== expected)
    throw new Error(`set-aside holds ${setAsideCount()} copies, not ${expected}`);
}

if (scenario === "moved") {
  plant();

  test("built-in: copies planted before start are moved aside, and Basecamp's own opens", async (app) => {
    await waitUntilMoved(app, copies.length, "every copy to be moved aside");
    await app.click("Package Manager", PMUI_SIDEBAR_SECTION);
    if (!(await packageManagerStore(app, 90000))) throw new Error("Package Manager never came up");
    expectBundledBackend("once Package Manager is open");
    expectNoCopyRuns("once Package Manager is open");
  });

  test("built-in: copies planted while it runs move on the next rescan, and Reopen restores Basecamp's own", async (app) => {
    plant();
    const anchor = await requireObject(app, "sidebar.section.2");
    await evalOn(app, anchor.id, "backend.refreshCoreModules()");
    await evalOn(app, anchor.id, "backend.refreshUiModules()");
    await waitUntilMoved(app, 2 * copies.length, "the new copies to be moved aside");
    expectNoCopyRuns("after the rescan");

    // The rescan pointed the running backend's entry at the planted copy, so
    // the backend was stopped: Package Manager shows its stopped panel.
    const reopen = await requireObject(app, "appStopped.reopen");
    // Reopen loads from the app list, which the refresh above fetches again.
    await sleep(3000);
    await evalOn(app, reopen.id, "clicked()");
    if (!(await packageManagerStore(app, 90000))) throw new Error("Package Manager did not come back after Reopen");
    expectBundledBackend("after Reopen");
    expectNoCopyRuns("after Reopen");
  });

  test("built-in: App Manager opens from Basecamp's own copy too", async (app) => {
    await app.click("Applications", PMUI_SIDEBAR_SECTION);
    await requireObject(app, "appManager.header", 90000);
    expectBundledBackend("once App Manager is open", appManager);
    expectNoCopyRuns("once App Manager is open");
  });
}

if (scenario === "stuck") {
  plant();
  // A file where set-aside would be: nothing can be moved there.
  writeFileSync(setAside, "");

  test("built-in: copies that cannot be moved stay, and both apps are refused", async (app) => {
    await requireObject(app, "sidebar.section.2");
    // The app list is fetched once the window is up; give it time to try.
    await sleep(5000);
    const left = stillPlanted();
    if (left.length !== copies.length) throw new Error(`expected every copy to stay, found ${left.join(", ")}`);
    await app.click("Package Manager", PMUI_SIDEBAR_SECTION);
    if (await packageManagerStore(app, 30000)) throw new Error("Package Manager opened from a copy that was not moved");
    await app.click("Applications", PMUI_SIDEBAR_SECTION);
    await sleep(10000);
    if (((await app.findByProperty("objectName", "appManager.header")).matches ?? []).length)
      throw new Error("App Manager opened from a copy that was not moved");
    expectNoCopyRuns("after asking for both apps");
  });
}

await run();
