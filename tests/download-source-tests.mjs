#!/usr/bin/env node
// ---------------------------------------------------------------------------
// Where apps download from, end to end: onboarding's choice, then what the
// App Manager and the Package Manager say about it in each state. The two
// views read the same downloader, and must agree.
//
// One fresh app and user dir per scenario, so onboarding runs each time.
// Runs online whatever the network (BASECAMP_FORCE_OFFLINE=0): a build
// sandbox has none, and offline every notice correctly stays quiet.
//
// Usage:
//   node tests/download-source-tests.mjs <app-binary> [name filter]
//
// Set LOGOS_QT_MCP to override the framework path. Default: ./result-mcp
// ---------------------------------------------------------------------------

import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { spawn } from "node:child_process";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";

const __dirname = dirname(fileURLToPath(import.meta.url));
const qtMcpRoot = process.env.LOGOS_QT_MCP || resolve(__dirname, "..", "result-mcp");
const { Inspector } = await import(resolve(qtMcpRoot, "test-framework/framework.mjs"));

const APP_BIN = process.argv[2];
const FILTER = process.argv[3] || "";
if (!APP_BIN) {
  console.error("Usage: node tests/download-source-tests.mjs <app-binary> [name filter]");
  process.exit(1);
}

const STORAGE_DOWN = "Logos Storage isn't running";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitUntil(check, ms, description) {
  const deadline = Date.now() + ms;
  let last;
  while (Date.now() < deadline) {
    try {
      const value = await check();
      if (value !== false && value !== undefined) return value;
    } catch (e) {
      last = e;
    }
    await sleep(500);
  }
  throw new Error(`timed out after ${ms / 1000}s waiting for ${description}`
                  + (last ? ` (${last.message})` : ""));
}

// A running Basecamp and what the scenarios ask of it.
class Basecamp {
  constructor(child, inspector) {
    this.child = child;
    this.inspector = inspector;
    this.logChunks = [];
    child.stdout.on("data", (d) => this.logChunks.push(d));
    child.stderr.on("data", (d) => this.logChunks.push(d));
  }

  static async launch(env = {}) {
    const userDir = mkdtempSync(join(tmpdir(), "basecamp-download-source-"));
    const child = spawn(APP_BIN, ["-platform", "offscreen", "--user-dir", userDir, "--new-instance"], {
      stdio: ["ignore", "pipe", "pipe"],
      env: {
        ...process.env,
        QT_QPA_PLATFORM: "offscreen",
        LOGOS_NO_SCHEME_REGISTER: "1",
        BASECAMP_FORCE_OFFLINE: "0",
        ...env,
      },
    });
    const inspector = new Inspector();
    await waitUntil(async () => { await inspector.connect(); return true; }, 30000, "the inspector");
    const app = new Basecamp(child, inspector);
    await waitUntil(() => app.find("onboardingView"), 30000, "onboarding");
    return app;
  }

  log() { return Buffer.concat(this.logChunks).toString("utf-8"); }

  async find(name) {
    const res = await this.inspector.send("findByProperty", { property: "objectName", value: name });
    return res.matches && res.matches.length > 0 ? res.matches[0].id : null;
  }

  async eval(name, expression) {
    const id = await this.find(name);
    if (!id) throw new Error(`no object named ${name}`);
    const res = await this.inspector.send("evaluate", { objectId: id, expression });
    if (res.error) throw new Error(`${name}: ${expression}: ${res.error}`);
    return res.result;
  }

  async json(name, expression) {
    return JSON.parse(await this.eval(name, `JSON.stringify(${expression})`));
  }

  async click(name) {
    const id = await waitUntil(() => this.find(name), 10000, name);
    await this.inspector.send("click", { objectId: id });
    await sleep(400);
  }

  // Basecamp's backend, once onboarding is over.
  backend(expression) { return this.eval("welcomePage.greeting", `backend.${expression}`); }

  async section(index) {
    await this.backend(`setCurrentActiveSectionIndex(${index})`);
    await sleep(500);
  }

  // --- Onboarding ---

  async letsGo() {
    await this.click("onboardingQuickSetupButton");
    await this.onboardingGone();
  }

  // Advanced setup with the sources given; on to the storage step if ticked.
  async advanced({ github, storage }) {
    await this.click("onboardingCustomSetupButton");
    if (!github) await this.click("onboardingGithubCard");
    if (!storage) await this.click("onboardingStorageCard");
    await this.click("onboardingDownloadsAdvanceButton");
  }

  async storageStepState() { return this.eval("onboardingStorageStep", "status"); }

  async finishStorageStep() {
    await waitUntil(async () => (await this.storageStepState()) === "running", 90000,
                    "Logos Storage to start in onboarding");
    await this.click("onboardingStorageAdvanceButton");
    await this.onboardingGone();
  }

  async onboardingGone() {
    await waitUntil(async () => (await this.find("onboardingView")) === null
                                && (await this.find("welcomePage.greeting")) !== null,
                    20000, "onboarding to close");
  }

  // --- What each view says ---

  async basecampState() {
    return {
      source: await this.backend("downloadSource"),
      storage: await this.backend("storageState"),
    };
  }

  async appManager() {
    await this.section(1);
    return this.json("appManager.downloadNotice", "({ shown: shown, title: title })");
  }

  async packageManager() {
    await this.section(2);
    await waitUntil(() => this.find("pmui.BackendStore"), 30000, "the Package Manager");
    return {
      ...(await this.json("pmui.BackendStore", "({ source: downloadSource, storage: storageState })")),
      ...(await this.json("pmui.downloadNotice", "({ shown: shown, title: title })")),
    };
  }

  async waitForStorage(state, ms = 90000) {
    await waitUntil(async () => (await this.backend("storageState")) === state, ms,
                    `Basecamp to see Logos Storage "${state}"`);
  }

  async unloadStorage() {
    await this.backend('unloadCoreModule("storage_module")');
    await this.backend('confirmUnloadCascade("storage_module")');
    await waitUntil(() => this.log().includes("Module unloaded: storage_module"), 10000,
                    "storage_module to unload");
  }

  async quit() {
    this.inspector.disconnect?.();
    this.child.kill("SIGTERM");
    await Promise.race([new Promise((r) => this.child.once("close", r)), sleep(10000)]);
    if (this.child.exitCode === null) this.child.kill("SIGKILL");
  }
}

function expectNotice(where, got, title) {
  if (title === null) {
    if (got.shown) throw new Error(`${where} shows "${got.title}", expected no notice`);
  } else if (!got.shown || got.title !== title) {
    throw new Error(`${where} shows ${got.shown ? `"${got.title}"` : "nothing"}, expected "${title}"`);
  }
}

function expectEqual(where, got, want) {
  if (got !== want) throw new Error(`${where} is ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`);
}

// --- Scenarios ---

const scenarios = [];
const scenario = (name, body, env) => scenarios.push({ name, body, env });

scenario("Let's go: both sources, storage running, no notice anywhere", async (app) => {
  await app.letsGo();
  await app.waitForStorage("running");
  expectEqual("Basecamp's source", (await app.basecampState()).source, "any");
  expectNotice("App Manager", await app.appManager(), null);
  const pm = await app.packageManager();
  expectEqual("Package Manager's source", pm.source, "any");
  await waitUntil(async () => (await app.packageManager()).storage === "running", 20000,
                  "the Package Manager to see Logos Storage running");
  expectNotice("Package Manager", await app.packageManager(), null);
});

scenario("GitHub only: storage never starts, no notice anywhere", async (app) => {
  await app.advanced({ github: true, storage: false });
  await app.onboardingGone();
  await sleep(3000);
  expectEqual("Basecamp's source", (await app.basecampState()).source, "http");
  if (app.log().includes("Started Storage node")) throw new Error("the storage node started");
  expectNotice("App Manager", await app.appManager(), null);
  const pm = await app.packageManager();
  expectEqual("Package Manager's source", pm.source, "http");
  expectNotice("Package Manager", pm, null);
});

scenario("Storage only: the storage step runs, then no notice anywhere", async (app) => {
  await app.advanced({ github: false, storage: true });
  await app.finishStorageStep();
  expectEqual("Basecamp's source", (await app.basecampState()).source, "logos");
  expectNotice("App Manager", await app.appManager(), null);
  await waitUntil(async () => (await app.packageManager()).storage === "running", 20000,
                  "the Package Manager to see Logos Storage running");
  expectNotice("Package Manager", await app.packageManager(), null);
});

scenario("Storage only, storage unloaded: both views say so", async (app) => {
  await app.advanced({ github: false, storage: true });
  await app.finishStorageStep();
  await app.packageManager();  // loaded now: opening it later brings storage back
  await app.section(3);
  await app.unloadStorage();
  await app.waitForStorage("failed", 15000);
  expectNotice("App Manager", await app.appManager(), STORAGE_DOWN);
  await waitUntil(async () => (await app.packageManager()).title === STORAGE_DOWN, 15000,
                  "the Package Manager to say Logos Storage isn't running");
});

// Opening the Package Manager loads package_downloader's dependencies, and with
// them the storage_module the user unloaded; the downloader starts the node
// again. Those changes go out while the view is still subscribing.
scenario("Storage only, storage unloaded, then the Package Manager opens: both clear", async (app) => {
  await app.advanced({ github: false, storage: true });
  await app.finishStorageStep();
  await app.section(3);
  await app.unloadStorage();
  await app.waitForStorage("failed", 15000);
  await app.packageManager();
  await app.waitForStorage("running");
  await waitUntil(async () => !(await app.packageManager()).shown, 20000,
                  "the Package Manager's notice to clear");
  expectEqual("Package Manager's storage state", (await app.packageManager()).storage, "running");
  expectNotice("App Manager", await app.appManager(), null);
});

scenario("Both sources, storage down: only the Package Manager mentions it", async (app) => {
  await app.letsGo();
  await app.waitForStorage("running");
  await app.packageManager();
  await app.section(3);
  await app.unloadStorage();
  await app.waitForStorage("failed", 15000);
  expectNotice("App Manager", await app.appManager(), null);
  await waitUntil(async () => (await app.packageManager()).title === STORAGE_DOWN, 15000,
                  "the Package Manager to say Logos Storage isn't running");
});

scenario("Back from the storage step to GitHub only: storage is let go", async (app) => {
  await app.advanced({ github: true, storage: true });
  await waitUntil(() => app.find("onboardingStorageStep"), 10000, "the storage step");
  await app.click("onboardingStorageBackButton");
  await app.click("onboardingStorageCard");
  await app.click("onboardingDownloadsAdvanceButton");
  await app.onboardingGone();
  await waitUntil(async () => (await app.basecampState()).source === "http", 15000,
                  "the source to become GitHub only");
  await waitUntil(() => app.log().includes("Module unloaded: storage_module"), 15000,
                  "storage_module to be let go");
  const pm = await app.packageManager();
  expectEqual("Package Manager's source", pm.source, "http");
  expectNotice("Package Manager", pm, null);
});

scenario("Offline: the toast says so, the views stay quiet", async (app) => {
  await app.advanced({ github: false, storage: false });  // nothing to start offline
  // Neither ticked keeps Continue disabled: GitHub only instead.
  await app.click("onboardingGithubCard");
  await app.click("onboardingDownloadsAdvanceButton");
  await app.onboardingGone();
  const toast = await app.json("basecamp.offlineToast", "({ visible: visible, title: title })");
  if (!toast.visible) throw new Error("the offline toast is not shown");
  expectNotice("App Manager", await app.appManager(), null);
  expectNotice("Package Manager", await app.packageManager(), null);
}, { BASECAMP_FORCE_OFFLINE: "1" });

// --- Runner ---

let failed = 0;
let ran = 0;
for (const s of scenarios) {
  if (FILTER && !s.name.includes(FILTER)) continue;
  ran++;
  process.stdout.write(`  ${s.name} ... `);
  const started = Date.now();
  let app = null;
  try {
    app = await Basecamp.launch(s.env);
    await s.body(app);
    console.log(`\x1b[32mPASS\x1b[0m (${((Date.now() - started) / 1000).toFixed(0)}s)`);
  } catch (e) {
    failed++;
    console.log(`\x1b[31mFAIL\x1b[0m\n    ${e.message}`);
    if (app && process.env.VERBOSE) console.log(app.log().split("\n").slice(-40).join("\n"));
  } finally {
    if (app) await app.quit();
    await sleep(1000);
  }
}
console.log(`\n${ran - failed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
