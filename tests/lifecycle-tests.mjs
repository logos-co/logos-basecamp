#!/usr/bin/env node
// ---------------------------------------------------------------------------
// logos-basecamp lifecycle tests — the nightly suite of the MCP test plan
// (B1–B31: install gates, failure paths, uninstall cascades, module IPC,
// restart).
//
// Every test gets a fresh app on its own --user-dir, so a test may install,
// uninstall, break or restart whatever it likes without touching the next
// one. The shared-instance model of ui-tests.mjs does not fit, and unlike
// shutdown-tests.mjs the tests act AFTER startup, so the epilogue scans the
// whole captured log for QML errors, not only the startup part.
//
// Per test, in order:
//   1. a fresh user dir, a fresh app (offscreen, --new-instance), stdout and
//      stderr captured, the inspector awaited;
//   2. first-run onboarding dismissed with "Let's go" (a test that seeds a
//      plugin before boot never sees it, so either state is accepted);
//   3. the body, with { app, child, userDir, log, note };
//   4. G-ERR  — no QML error anywhere in the captured output;
//      G-ALIVE — the inspector still answers within 5 s;
//      G-EXIT  — SIGTERM, exit code 0 within 25 s (SIGKILL as the fallback).
// The user dir is kept under $TMPDIR for post-mortem.
//
// Usage:
//   node tests/lifecycle-tests.mjs <app-binary> [name filter]
//
// Set LOGOS_QT_MCP to override the framework path (nix builds set this
// automatically). Default: ./result-mcp. LGX_CLI names the lgx CLI the
// fixtures are built with; LGX_VARIANT their variant (a dev build accepts
// only "<platform>-dev"). QML_INSPECTOR_PORT is passed through (default 3768):
// tests run one at a time, and the next app is spawned only after the
// previous one has exited.
// ---------------------------------------------------------------------------

import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import {
  assertResponsive,
  expectAbsent,
  findByObjectName,
  installViaPmu,
  makeUserDir,
  scanForQmlErrors,
  sleep,
  spawnBasecamp,
  waitForExit,
  waitForPort,
} from "./fixtures/harness.mjs";
import { makeLgx, makeLifecycleFixtures, writeInstalledPlugin } from "./fixtures/lgx.mjs";
import { serveRepository } from "./fixtures/https-repo.mjs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(__dirname, "..");
const qtMcpRoot = process.env.LOGOS_QT_MCP || resolve(projectRoot, "result-mcp");
const { Inspector } = await import(resolve(qtMcpRoot, "test-framework/framework.mjs"));

const APP_BIN = process.argv[2];
const FILTER = process.argv[3] || "";
if (!APP_BIN) {
  console.error("Usage: node tests/lifecycle-tests.mjs <app-binary> [name filter]");
  process.exit(1);
}

const HOST = process.env.QML_INSPECTOR_HOST || "localhost";
const PORT = parseInt(process.env.QML_INSPECTOR_PORT || "3768", 10);
const INSPECTOR_WAIT_MS = 30000;
// Onboarding and the welcome page come up after the core has started.
const FIRST_RUN_WAIT_MS = 30000;
// The Package Manager spawns a ui-host and waits on its ready handshake
// (PluginLoader gives that 30 s) before its QML compiles.
const PMUI_WAIT_MS = 45000;
// App Manager the same, and then its catalog.
const APP_MANAGER_WAIT_MS = 60000;
// Above the longest bounded teardown a single module can cost (see
// shutdown-tests.mjs).
const SHUTDOWN_WAIT_MS = 25000;

// ---------------------------------------------------------------------------
// Fixtures: built once per run, named in one place (tests/fixtures/lgx.mjs).
// ---------------------------------------------------------------------------
const fixtureDir = mkdtempSync(join(tmpdir(), "basecamp-lifecycle-lgx-"));
const FIXTURES = makeLifecycleFixtures(fixtureDir);

// ---------------------------------------------------------------------------
// The app as a test body sees it: a thin layer over the inspector protocol.
//
// Clicks are signal-level (callMethod "clicked", the same chain a real click
// runs), as in ui-tests.mjs: the QML lives in offscreen QQuickWindows where
// coordinate hit-testing is unreliable. evaluate round-trips primitives only:
// one property per call, JSON for anything structured.
// ---------------------------------------------------------------------------
class App {
  constructor(inspector) {
    this.inspector = inspector;
  }

  async send(command, params) {
    const res = await this.inspector.send(command, params);
    if (res && res.error) throw new Error(`${command} failed: ${res.error}`);
    return res;
  }

  findByObjectName(name) {
    return findByObjectName(this.inspector, name);
  }

  async findByProperty(property, value) {
    return (await this.send("findByProperty", { property, value })).matches ?? [];
  }

  async findByType(typeName) {
    return (await this.send("findByType", { typeName })).matches ?? [];
  }

  getTree({ depth = 50 } = {}) {
    return this.send("getTree", { depth });
  }

  // Waits for an objectName to appear; returns the match.
  async requireObject(objectName, timeout = 10000) {
    let obj = null;
    await this.waitFor(async () => {
      obj = await this.findByObjectName(objectName);
      if (!obj) throw new Error(`${objectName} not in the QML tree`);
    }, { timeout, interval: 500, description: `${objectName} to exist` });
    return obj;
  }

  // `target`: a match ({ id }), an objectName, or a raw object id.
  async evalOn(target, expression) {
    let objectId = target;
    let unnamed = false;
    if (target && typeof target === "object") {
      objectId = target.id;
    } else if (typeof target === "string") {
      const obj = await this.findByObjectName(target);
      if (obj) objectId = obj.id;
      else unnamed = true;
    }
    const res = await this.inspector.send("evaluate", { objectId, expression });
    if (res.error) {
      // A string that named nothing went through as a raw id; say so, or the
      // failure reads as a bad expression.
      const hint = unnamed ? ` (no object named "${target}" in the QML tree)` : "";
      throw new Error(`evaluate("${expression}") failed: ${res.error}${hint}`);
    }
    return res.result;
  }

  // click(objectName) emits the object's clicked(); click(text, { type,
  // exact }) picks the instance of `type` whose text matches (exact, or a
  // case-insensitive substring) — how the sidebar SECTION buttons are
  // addressed, since a bare text search would land on MainContainer's
  // "Loading Package Manager…" placeholder first (ui-tests.mjs,
  // sidebarSection).
  async click(name, opts = {}) {
    const { type = null, exact = false } = opts;
    let target = null;
    if (type) {
      const wanted = name.toLowerCase();
      for (const m of await this.findByType(type)) {
        const text = await this.evalOn(m, "text");
        if (typeof text !== "string") continue;
        if (exact ? text === name : text.toLowerCase().includes(wanted)) {
          target = m;
          break;
        }
      }
      if (!target) throw new Error(`no ${type} with text "${name}"`);
    } else {
      target = await this.findByObjectName(name);
      if (!target) throw new Error(`${name} not in the QML tree`);
    }
    await this.send("callMethod", { objectId: target.id, method: "clicked" });
  }

  async waitFor(fn, opts = {}) {
    const { timeout = 10000, interval = 500, description = "condition" } = opts;
    const deadline = Date.now() + timeout;
    let last = null;
    for (;;) {
      try {
        return await fn();
      } catch (e) {
        last = e;
      }
      if (Date.now() >= deadline) {
        throw new Error(`timed out after ${timeout}ms waiting for ${description}` +
                        (last ? ` (${last.message})` : ""));
      }
      await sleep(interval);
    }
  }

  // Every text must render somewhere: as a whole `text` property, or as a
  // substring of one in the tree dump.
  async expectTexts(texts) {
    const missing = [];
    let treeStr = null;
    for (const text of texts) {
      if ((await this.findByProperty("text", text)).length > 0) continue;
      if (treeStr === null) treeStr = JSON.stringify(await this.getTree({ depth: 60 }));
      if (treeStr.includes(JSON.stringify(text).slice(1, -1))) continue;
      missing.push(text);
    }
    if (missing.length > 0) {
      throw new Error(`text(s) not rendered: ${missing.map((t) => JSON.stringify(t)).join(", ")}`);
    }
  }
}

// A fresh user dir boots into first-run onboarding (app/OnboardingState.cpp:
// skipped only when plugins/ or modules/ has an entry, an earlier session's
// log exists, or a repositories.json was saved). A test that seeds a plugin
// before boot therefore lands on the welcome page directly.
async function passOnboarding(app) {
  let onboarding = false;
  await app.waitFor(async () => {
    if (await app.findByObjectName("onboardingView")) { onboarding = true; return; }
    if (await app.findByObjectName("welcomePage.greeting")) return;
    throw new Error("neither onboardingView nor welcomePage.greeting is in the QML tree");
  }, { timeout: FIRST_RUN_WAIT_MS, interval: 500, description: "onboarding or the welcome page" });
  if (!onboarding) return;
  await app.requireObject("onboardingQuickSetupButton");
  await app.click("onboardingQuickSetupButton");
  await app.waitFor(async () => {
    if (await app.findByObjectName("onboardingView")) throw new Error("onboardingView still present");
    if (!(await app.findByObjectName("welcomePage.greeting"))) throw new Error("welcomePage.greeting absent");
  }, { timeout: 20000, interval: 500, description: "onboarding to close" });
}

// ---------------------------------------------------------------------------
// Runner. Body-return protocol: throw = FAIL; return skipTest(reason) = SKIP;
// else PASS. opts: { xfail } — loud on unexpected pass; { skip } — not run;
// { env } — for the app.
// ---------------------------------------------------------------------------
class SkipTest {
  constructor(reason) { this.reason = reason; }
}
function skipTest(reason) { return new SkipTest(reason); }

// SIGTERM, then the orderly exit; SIGKILL if it does not come.
async function shutDown(child) {
  if (child.exitCode !== null || child.signalCode !== null) {
    return { code: child.exitCode, signal: child.signalCode };
  }
  child.kill("SIGTERM");
  const exit = await waitForExit(child, SHUTDOWN_WAIT_MS);
  if (exit) return exit;
  child.kill("SIGKILL");
  await waitForExit(child, 2000);
  return null;
}

async function runTest(name, body, opts = {}) {
  if (FILTER && !name.includes(FILTER)) return null;
  process.stdout.write(`  ${name} ... `);
  if (opts.skip) {
    console.log(`\x1b[33mSKIP\x1b[0m — ${opts.skip}`);
    return "skip";
  }

  const userDir = makeUserDir("basecamp-lifecycle");
  if (opts.seed) opts.seed(userDir);
  const child = spawnBasecamp(APP_BIN, userDir, { env: opts.env ?? {} });
  const logChunks = [];
  child.stdout.on("data", (d) => logChunks.push(d));
  child.stderr.on("data", (d) => logChunks.push(d));
  const log = () => Buffer.concat(logChunks).toString("utf-8");
  const notes = [];
  const note = (text) => notes.push(text);

  let outcome = "pass";
  let err = null;
  let skipReason = null;
  const inspector = new Inspector();
  let connected = false;
  try {
    await waitForPort(HOST, PORT, INSPECTOR_WAIT_MS, child);
    await inspector.connect();
    connected = true;
    const app = new App(inspector);
    await passOnboarding(app);
    const ret = await body({ app, child, userDir, log, note });
    if (ret instanceof SkipTest) {
      outcome = "skip";
      skipReason = ret.reason;
    } else {
      // G-ERR over the whole run: lifecycle tests act after startup.
      const qmlErrors = scanForQmlErrors(log());
      if (qmlErrors.length > 0) {
        throw new Error(`G-ERR: QML error(s):\n  ${qmlErrors.join("\n  ")}`);
      }
      await assertResponsive(app); // G-ALIVE
    }
  } catch (e) {
    outcome = "fail";
    err = e;
  }

  if (connected) {
    try { inspector.disconnect?.(); } catch { /* already gone */ }
  }
  // G-EXIT: every test ends with the app exited with code 0.
  const exit = await shutDown(child);
  if (outcome === "pass") {
    if (!exit) {
      outcome = "fail";
      err = new Error(`G-EXIT: did not exit within ${SHUTDOWN_WAIT_MS}ms of SIGTERM (killed)`);
    } else if (exit.signal) {
      outcome = "fail";
      err = new Error(`G-EXIT: terminated by signal ${exit.signal} instead of orderly exit`);
    } else if (exit.code !== 0) {
      outcome = "fail";
      err = new Error(`G-EXIT: exit code ${exit.code}, want 0`);
    }
  }

  if (outcome === "pass" && opts.xfail) {
    outcome = "fail";
    err = new Error(
      `XPASS: test passed but is marked xfail (${opts.xfail}) — the fix ` +
      `landed, remove the xfail marker`);
  } else if (outcome === "fail" && opts.xfail) {
    console.log(`\x1b[33mXFAIL\x1b[0m (${opts.xfail}) — ${err.message}`);
    for (const n of notes) console.log(`    ${n}`);
    return "pass";
  }

  if (outcome === "pass") {
    console.log("\x1b[32mOK\x1b[0m");
  } else if (outcome === "skip") {
    console.log(`\x1b[33mSKIP\x1b[0m — ${skipReason}`);
  } else {
    console.log(`\x1b[31mFAIL\x1b[0m — ${err.message}`);
  }
  for (const n of notes) console.log(`    ${n}`);
  if (outcome === "fail") {
    console.log(`    user dir kept for post-mortem: ${userDir}`);
    process.stderr.write("--- app output ---\n");
    process.stderr.write(log());
    process.stderr.write("--- end app output ---\n");
  }
  return outcome;
}

// ---------------------------------------------------------------------------
// Shared steps
// ---------------------------------------------------------------------------
function assertEq(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}=${JSON.stringify(actual)} (expected ${JSON.stringify(expected)})`);
  }
}

// Pins a click to a sidebar SECTION button (ui-tests.mjs, sidebarSection).
const sidebarSection = { exact: true, type: "SidebarCircleButton" };

// Opens the Package Manager section and waits for PMUI's own header text —
// the one string basecamp's QML never renders — and for pmui.BackendStore,
// which is created lazily on first activation and is what installs go through.
async function openPackageManager(app) {
  await app.click("Package Manager", sidebarSection);
  await app.waitFor(async () => { await app.expectTexts(["Manage your plugins and packages."]); },
    { timeout: PMUI_WAIT_MS, interval: 500, description: "the Package Manager view to load" });
  await app.requireObject("pmui.BackendStore", PMUI_WAIT_MS);
}

// The dialog instances are always in the tree (OverlayDialogs.qml) and only
// `visible` flips; a closed one keeps the texts it last rendered.
const INSTALL_GATE = "confirmationDialog.installGate";

async function waitGateHidden(app, timeout = 5000) {
  const gate = await app.requireObject(INSTALL_GATE);
  await app.waitFor(async () => {
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible`);
  }, { timeout, interval: 100, description: "the install gate to close" });
}

// Title and body of an OPEN gate: its title, and the texts its header and
// content render, the body being the LogosText named confirmationDialog.body.
async function gateTexts(app, gate) {
  const nodes = JSON.parse(await app.evalOn(gate, `(() => {
    const out = [];
    const walk = (node) => {
      if (!node) return;
      if (typeof node.text === "string")
        out.push({ text: node.text, objectName: node.objectName || "" });
      const kids = node.children;
      if (!kids || typeof kids.length !== "number") return;
      for (let i = 0; i < kids.length; i += 1) walk(kids[i]);
    };
    walk(header);
    walk(contentItem);
    return JSON.stringify(out);
  })()`));
  const body = nodes.find((n) => n.objectName === "confirmationDialog.body");
  const title = await app.evalOn(gate, "title");
  return { title: typeof title === "string" ? title : "", body: body ? body.text : "", texts: nodes.map((n) => n.text) };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
console.log(`\nlogos-basecamp lifecycle tests (${process.platform})\n`);
const suiteStart = Date.now();
const results = [];

// B1. Cancelling the install gate: no files, no tile, no row, and the pending
// install slot (PackageCoordinator::cancelInstallGate clears
// m_pendingInstallRequestId) is free for the next request.
results.push(await runTest("install: cancelling the install gate leaves nothing behind",
  async ({ app, userDir, note }) => {
    await openPackageManager(app);
    const greeting = await app.requireObject("welcomePage.greeting");
    const rowCount = () => app.evalOn(greeting, "backend.uiModulesModel.rowCount()");
    const rowsBefore = await rowCount();
    if (typeof rowsBefore !== "number") {
      throw new Error(`uiModulesModel.rowCount() read as ${JSON.stringify(rowsBefore)}`);
    }
    const pluginDir = join(userDir, "plugins", "test_qml_only");
    const gate = await app.requireObject(INSTALL_GATE);
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible before any install`);

    // The gate: its title, and a body that leads with the package and the
    // version. The package is named by its display name when one is known,
    // by its raw name otherwise; the clause after the version says who
    // signed it once the check has run (this fixture: nobody).
    const BODY_RE = /^Install '(Test QML Only|test_qml_only)' v0\.1\.1([^.]*)\.(?: |$)/;
    await installViaPmu(app, FIXTURES.A_0_1_1, {
      confirm: false,
      whileOpen: async (open) => {
        const { title, body, texts } = await gateTexts(app, open);
        if (!texts.includes("Install Package?")) {
          throw new Error(`open gate renders no "Install Package?" (texts: ${JSON.stringify(texts)})`);
        }
        assertEq(title, "Install Package?", "install gate title");
        if (!BODY_RE.test(body)) {
          throw new Error(`install gate body ${JSON.stringify(body)} does not start ` +
                          `"Install 'Test QML Only' v0.1.1."`);
        }
        note(`gate body: ${body}`);
      },
    });
    await waitGateHidden(app, 5000);

    await expectAbsent(app, "sidebar.app.test_qml_only", 10000);
    if (existsSync(pluginDir)) throw new Error(`${pluginDir} exists after cancel`);
    assertEq(await rowCount(), rowsBefore, "uiModulesModel.rowCount() after cancel");

    // A second request opens a NEW gate: the first cancel released the
    // pending slot, or this one would be refused as "confirmation in flight".
    await installViaPmu(app, FIXTURES.A_0_1_1, {
      confirm: false,
      whileOpen: async (open) => {
        assertEq(await app.evalOn(open, "visible"), true, `${INSTALL_GATE} visible on the second request`);
        const { title } = await gateTexts(app, open);
        assertEq(title, "Install Package?", "second install gate title");
      },
    });
    await waitGateHidden(app, 5000);
    await expectAbsent(app, "sidebar.app.test_qml_only", 3000);
    if (existsSync(pluginDir)) throw new Error(`${pluginDir} exists after the second cancel`);
    assertEq(await rowCount(), rowsBefore, "uiModulesModel.rowCount() after the second cancel");
  }));

// A click on an app that isn't installed shows its details beside the list;
// their Install asks Basecamp. The panel's button is there, hidden, before the
// details come: wait until it offers Install.
async function clickDetailsInstall(app) {
  const install = await app.requireObject("appManager.details.action", 30000);
  await app.waitFor(async () => {
    assertEq(await app.evalOn(install, "visible && enabled ? text : ''"), "Install",
             "the details' Install");
  }, { timeout: 30000, interval: 300, description: "the app's details to offer Install" });
  await app.send("callMethod", { objectId: install.id, method: "clicked" });
}

// Picks `version` in a details row's version picker (`row`, its
// appManager.details.package.* object), as the person does: by its place in
// the list it shows. What the pick sends is App Manager's to say.
async function pickVersion(app, row, version) {
  const picked = await app.evalOn(row, `(() => {
    const find = (node) => {
      if (!node) return null;
      if (node.objectName === "packageRow.versionPicker") return node;
      const kids = node.children;
      for (let i = 0; kids && i < kids.length; i += 1) {
        const found = find(kids[i]);
        if (found) return found;
      }
      return null;
    };
    let picker = null;
    for (let i = 0; !picker && i < children.length; i += 1) picker = find(children[i]);
    const index = picker ? Array.from(picker.model).indexOf(${JSON.stringify(version)}) : -1;
    if (index < 0) return false;
    picker.activated(index);
    return true;
  })()`);
  if (picked !== true) throw new Error(`the row's version picker has no ${version}`);
}

// B2. The first install from App Manager, end to end: its backend asks
// Basecamp (basecamp.packages.confirm_install, through intents_module), the
// gate names App Manager as the asker, and on Install the backend downloads
// exactly the approved release from the repository and installs it. Then the
// way back: App Manager asks to uninstall it (confirm_uninstall), Basecamp
// plans the app's removal, and the backend removes what the answer names.
let repo = null;
try {
  repo = await serveRepository(mkdtempSync(join(tmpdir(), "basecamp-lifecycle-repo-")),
                               [FIXTURES.A_0_1_1]);
} catch (e) {
  repo = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("install and uninstall: App Manager asks, Basecamp's gate approves, App Manager does it",
  async ({ app, userDir, note }) => {
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(repo.url)})`);
    await app.evalOn(greeting, `backend.setCurrentActiveSectionIndex(1)`);

    // A click shows what installing it brings; Install there asks Basecamp.
    const tile = await app.requireObject("appManager.app.test_qml_only", APP_MANAGER_WAIT_MS);
    await app.send("callMethod", { objectId: tile.id, method: "clicked" });
    await clickDetailsInstall(app);

    const gate = await app.requireObject(INSTALL_GATE);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(gate, "visible"), true, `${INSTALL_GATE} visible`);
    }, { timeout: 30000, interval: 300, description: "the install gate to open" });
    assertEq(await app.evalOn(gate, "requesterName"), "app_manager_ui", "the gate's requester");
    const { title, body } = await gateTexts(app, gate);
    assertEq(title, "Install App?", "the gate's title, in App Manager's words");
    note(`gate body: ${body}`);

    const confirm = await app.requireObject(`${INSTALL_GATE}.confirm`);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(confirm, "enabled"), true, "Install enabled");
    }, { timeout: 30000, interval: 300, description: "the gate's trust check" });
    await app.send("callMethod", { objectId: confirm.id, method: "clicked" });
    await waitGateHidden(app, 5000);

    await app.requireObject("sidebar.app.test_qml_only", 60000);
    if (!existsSync(join(userDir, "plugins", "test_qml_only")))
      throw new Error("test_qml_only is not in the user's plugins after the install");
    await app.waitFor(async () => {
      const installed = await app.findByObjectName("appManager.app.test_qml_only");
      assertEq(installed && await app.evalOn(installed, "appData.isInstalled"), true,
               "App Manager's tile installed");
    }, { timeout: 30000, interval: 500, description: "App Manager's tile to say installed" });

    const installedTile = await app.requireObject("appManager.app.test_qml_only");
    await app.evalOn(installedTile, "detailsRequested(appData.name, appData.repositoryUrl)");
    const uninstall = await app.requireObject("appManager.details.uninstall", 30000);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(uninstall, "visible"), true, "the details page's Uninstall");
    }, { timeout: 30000, interval: 300, description: "the details page to offer Uninstall" });
    await app.send("callMethod", { objectId: uninstall.id, method: "clicked" });

    const dialog = await app.requireObject("uninstallDialog");
    await app.waitFor(async () => {
      assertEq(await app.evalOn(dialog, "visible"), true, "uninstallDialog visible");
      assertEq(await app.evalOn(dialog, "plan.loading === true"), false, "the plan, read");
    }, { timeout: 30000, interval: 300, description: "the uninstall confirmation to open" });
    assertEq(await app.evalOn(dialog, "plan.kind"), "app", "the plan removes an app");
    assertEq(await app.evalOn(dialog, "plan.requester"), "app_manager_ui", "the uninstall's requester");
    assertEq(await app.evalOn(dialog, "JSON.stringify(plan.batch)"), JSON.stringify(["test_qml_only"]),
             "what it removes");
    const remove = await app.requireObject("uninstallDialog.confirmButton");
    await app.send("callMethod", { objectId: remove.id, method: "clicked" });

    await app.waitFor(async () => {
      if (existsSync(join(userDir, "plugins", "test_qml_only")))
        throw new Error("test_qml_only is still in the user's plugins");
      if (await app.findByObjectName("sidebar.app.test_qml_only"))
        throw new Error("test_qml_only is still in the sidebar");
    }, { timeout: 60000, interval: 500, description: "test_qml_only to be removed" });
  }, repo.skip ? { skip: repo.skip } : { env: repo.env }));
if (repo.close) await repo.close();

// B3. An app App Manager removes takes with it what only it needed: Basecamp
// plans both, says so, and its answer names both, which the backend removes.
results.push(await runTest("uninstall: App Manager removes an app and what only it needed",
  async ({ app, userDir }) => {
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setCurrentActiveSectionIndex(1)`);
    const tile = await app.requireObject("appManager.app.orphan_app", APP_MANAGER_WAIT_MS);
    await app.evalOn(tile, "detailsRequested(appData.name, appData.repositoryUrl)");
    const uninstall = await app.requireObject("appManager.details.uninstall", 30000);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(uninstall, "visible"), true, "the details page's Uninstall");
    }, { timeout: 30000, interval: 300, description: "the details page to offer Uninstall" });
    await app.send("callMethod", { objectId: uninstall.id, method: "clicked" });

    const dialog = await app.requireObject("uninstallDialog");
    await app.waitFor(async () => {
      assertEq(await app.evalOn(dialog, "visible"), true, "uninstallDialog visible");
      assertEq(await app.evalOn(dialog, "plan.loading === true"), false, "the plan, read");
    }, { timeout: 30000, interval: 300, description: "the uninstall confirmation to open" });
    assertEq(await app.evalOn(dialog, "plan.kind"), "app", "the plan removes an app");
    assertEq(await app.evalOn(dialog, "JSON.stringify(plan.batch)"),
             JSON.stringify(["orphan_app", "orphan_dep"]), "what it removes");
    const remove = await app.requireObject("uninstallDialog.confirmButton");
    await app.send("callMethod", { objectId: remove.id, method: "clicked" });

    await app.waitFor(async () => {
      for (const name of ["orphan_app", "orphan_dep"]) {
        if (existsSync(join(userDir, "plugins", name)))
          throw new Error(`${name} is still in the user's plugins`);
      }
    }, { timeout: 60000, interval: 500, description: "the app and its dependency to be removed" });
  }, { seed: (userDir) => {
    writeInstalledPlugin(userDir, { name: "orphan_dep" });
    writeInstalledPlugin(userDir, { name: "orphan_app", dependencies: ["orphan_dep"] });
  } }));

// B4. basecamp#427 through App Manager: an app whose own release is on disk,
// under a dependency republished at the same version with other contents. Its
// Reinstall fetches only that dependency, counted at the repository.
let drift = null;
try {
  const dir = mkdtempSync(join(tmpdir(), "basecamp-lifecycle-drift-"));
  const lgxDir = join(dir, "lgx");
  mkdirSync(lgxDir);
  const pkg = (name, file, extra = {}) => makeLgx({
    outFile: join(lgxDir, file), name, version: "1.0.0", variant: process.env.LGX_VARIANT, ...extra });
  const packages = [pkg("drift_dep_a", "drift_dep_a.lgx"), pkg("drift_dep_b", "drift_dep_b.lgx"),
                    pkg("drift_app", "drift_app.lgx", { dependencies: ["drift_dep_a", "drift_dep_b"] })];
  const republished = pkg("drift_dep_b", "drift_dep_b-2.lgx", { description: "drift_dep_b, republished" });
  drift = { repo: await serveRepository(join(dir, "repo"), packages), republished };
} catch (e) {
  drift = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("reinstall: a dependency that drifted is all App Manager fetches",
  async ({ app }) => {
    const { repo, republished } = drift;
    const fetched = () => repo.requests.filter((path) => path.endsWith(".lgx"));
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(repo.url)})`);
    await app.evalOn(greeting, `backend.setCurrentActiveSectionIndex(1)`);

    const confirmGate = async () => {
      const gate = await app.requireObject(INSTALL_GATE);
      await app.waitFor(async () => {
        assertEq(await app.evalOn(gate, "visible"), true, `${INSTALL_GATE} visible`);
      }, { timeout: 30000, interval: 300, description: "the install gate to open" });
      const confirm = await app.requireObject(`${INSTALL_GATE}.confirm`);
      await app.waitFor(async () => {
        assertEq(await app.evalOn(confirm, "enabled"), true, "Install enabled");
      }, { timeout: 30000, interval: 300, description: "the gate's trust check" });
      await app.send("callMethod", { objectId: confirm.id, method: "clicked" });
      await waitGateHidden(app, 5000);
    };

    const tile = await app.requireObject("appManager.app.drift_app", APP_MANAGER_WAIT_MS);
    await app.send("callMethod", { objectId: tile.id, method: "clicked" });
    await clickDetailsInstall(app);
    await confirmGate();
    await app.requireObject("sidebar.app.drift_app", 60000);
    assertEq(JSON.stringify(fetched().sort()),
             JSON.stringify(["/drift_app.lgx", "/drift_dep_a.lgx", "/drift_dep_b.lgx"]), "the first install's downloads");

    repo.republish(republished);
    const reload = await app.requireObject("appManager.reloadButton");
    await app.send("callMethod", { objectId: reload.id, method: "clicked" });
    const before = fetched().length;
    await app.evalOn(await app.requireObject("appManager.app.drift_app"), "openDetails()");
    const action = await app.requireObject("appManager.details.action", 30000);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(action, "text"), "Reinstall", "the details page's action");
    }, { timeout: 60000, interval: 500, description: "the drifted dependency to show as Reinstall" });
    await app.send("callMethod", { objectId: action.id, method: "clicked" });
    await confirmGate();
    await app.waitFor(async () => {
      assertEq(await app.evalOn(action, "visible"), false, "the details page's action, once reinstalled");
    }, { timeout: 60000, interval: 500, description: "the reinstall to finish" });
    assertEq(JSON.stringify(fetched().slice(before)), JSON.stringify(["/drift_dep_b-2.lgx"]),
             "the reinstall's downloads");
  }, drift.skip ? { skip: drift.skip } : { env: drift.repo.env }));
if (drift.repo) await drift.repo.close();

// B5. A version picked in App Manager's details for a package the app needs
// goes to Basecamp with the install: its dialog shows that version, and it is
// the one fetched.
let pinned = null;
try {
  const dir = mkdtempSync(join(tmpdir(), "basecamp-lifecycle-pin-"));
  const lgxDir = join(dir, "lgx");
  mkdirSync(lgxDir);
  const pkg = (name, file, version, extra = {}) => makeLgx({
    outFile: join(lgxDir, file), name, version, variant: process.env.LGX_VARIANT, ...extra });
  const packages = [pkg("pin_dep", "pin_dep-1.0.0.lgx", "1.0.0"),
                    pkg("pin_dep", "pin_dep-1.1.0.lgx", "1.1.0"),
                    pkg("pin_app", "pin_app.lgx", "1.0.0", { dependencies: ["pin_dep"] })];
  pinned = { repo: await serveRepository(join(dir, "repo"), packages) };
} catch (e) {
  pinned = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("pick: the version picked in App Manager for what an app needs is what installs",
  async ({ app }) => {
    const { repo } = pinned;
    const fetched = () => repo.requests.filter((p) => p.endsWith(".lgx"));
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(repo.url)})`);
    await app.evalOn(greeting, `backend.setCurrentActiveSectionIndex(1)`);

    const tile = await app.requireObject("appManager.app.pin_app", APP_MANAGER_WAIT_MS);
    await app.send("callMethod", { objectId: tile.id, method: "clicked" });
    // The details' rows are made again as they change: found afresh each time.
    const pinDep = () => app.requireObject("appManager.details.package.pin_dep", 30000);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(await pinDep(), "row.version"), "1.1.0", "pin_dep's newest, before a pick");
    }, { timeout: 30000, interval: 300, description: "pin_dep in the details" });
    await pickVersion(app, await pinDep(), "1.0.0");
    await app.waitFor(async () => {
      assertEq(await app.evalOn(await pinDep(), "row.version"), "1.0.0", "pin_dep at the version picked");
    }, { timeout: 30000, interval: 300, description: "the pick to show in the details" });

    await clickDetailsInstall(app);
    const gate = await app.requireObject(INSTALL_GATE);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(gate, "visible"), true, `${INSTALL_GATE} visible`);
    }, { timeout: 30000, interval: 300, description: "the install gate to open" });
    await app.waitFor(async () => {
      const change = await app.requireObject("confirmationDialog.change.pin_dep", 30000);
      const text = String(await app.evalOn(change, "text"));
      if (!text.includes("v1.0.0") || text.includes("v1.1.0"))
        throw new Error(`the gate's pin_dep row says ${JSON.stringify(text)}`);
    }, { timeout: 30000, interval: 300, description: "the gate to show pin_dep at 1.0.0" });
    const confirm = await app.requireObject(`${INSTALL_GATE}.confirm`);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(confirm, "enabled"), true, "Install enabled");
    }, { timeout: 30000, interval: 300, description: "the gate's trust check" });
    await app.send("callMethod", { objectId: confirm.id, method: "clicked" });
    await waitGateHidden(app, 5000);

    await app.requireObject("sidebar.app.pin_app", 60000);
    assertEq(JSON.stringify(fetched().sort()), JSON.stringify(["/pin_app.lgx", "/pin_dep-1.0.0.lgx"]),
             "the install's downloads");
  }, pinned.skip ? { skip: pinned.skip } : { env: pinned.repo.env }));
if (pinned.repo) await pinned.repo.close();

// B6. Leaving Applications or Package Manager for another section starts it
// afresh next time: its details closed and its search empty. Each app sees
// the leaving only as its view hidden behind the section Basecamp shows.
results.push(await runTest("leave: Applications and Package Manager start afresh once left",
  async ({ app }) => {
    const greeting = await app.requireObject("welcomePage.greeting");
    const views = [
      { name: "Applications", prefix: "appManager",
        open: () => app.evalOn(greeting, "backend.setCurrentActiveSectionIndex(1)") },
      { name: "Package Manager", prefix: "pmui", open: () => openPackageManager(app) },
    ];
    for (const view of views) {
      await view.open();
      const panel = await app.requireObject(`${view.prefix}.detailsPanel`, APP_MANAGER_WAIT_MS);
      const header = await app.requireObject(`${view.prefix}.header`);
      await app.evalOn(panel, "panelOpen = true");
      await app.evalOn(header, "openSearch('zzz')");
      assertEq(await app.evalOn(header, "searchText"), "zzz", `${view.name}'s search, before leaving`);
      assertEq(await app.evalOn(panel, "panelOpen"), true, `${view.name}'s details, before leaving`);

      await app.evalOn(greeting, "backend.setCurrentActiveSectionIndex(0)");
      await view.open();
      await app.waitFor(async () => {
        assertEq(await app.evalOn(panel, "panelOpen"), false, `${view.name}'s details`);
        assertEq(await app.evalOn(header, "searchText"), "", `${view.name}'s search`);
      }, { timeout: 10000, interval: 300, description: `${view.name} to start afresh` });
    }
  }));

// B7. Basecamp's own request to install an app (a Welcome page tile,
// openApp: apps.install) reaches App Manager through intents_module under
// Basecamp's name, main_ui: the only requester App Manager asks the gate for
// on the request itself. The app's details come up in Applications, and the
// gate asks as App Manager's. Another app's request only shows the app (App
// Manager's own tests).
let own = null;
try {
  own = { repo: await serveRepository(mkdtempSync(join(tmpdir(), "basecamp-lifecycle-own-")),
                                      [FIXTURES.A_0_1_1]) };
} catch (e) {
  own = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("own request: Basecamp's Install shows the app in App Manager, which asks the gate",
  async ({ app, userDir }) => {
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(own.repo.url)})`);
    // App Manager has the app in its catalogue before it is asked.
    await app.evalOn(greeting, "backend.setCurrentActiveSectionIndex(1)");
    await app.requireObject("appManager.app.test_qml_only", APP_MANAGER_WAIT_MS);
    await app.evalOn(greeting, "backend.setCurrentActiveSectionIndex(0)");

    await app.evalOn(greeting, `(backend.openApp("test_qml_only", "", true), "sent")`);
    const gate = await app.requireObject(INSTALL_GATE);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(gate, "visible"), true, `${INSTALL_GATE} visible`);
    }, { timeout: 30000, interval: 300, description: "the install gate to open" });
    assertEq(await app.evalOn(gate, "requesterName"), "app_manager_ui", "the gate's requester");
    assertEq(await app.evalOn(greeting, "backend.currentActiveSectionIndex"), 1, "the section in front");
    await app.waitFor(async () => {
      const name = await app.requireObject("appManager.details.name", 10000);
      assertEq(await app.evalOn(name, "text"), "Test QML Only", "the app App Manager shows");
    }, { timeout: 30000, interval: 300, description: "App Manager to show the app" });

    const cancel = await app.requireObject(`${INSTALL_GATE}.cancel`);
    await app.send("callMethod", { objectId: cancel.id, method: "clicked" });
    await waitGateHidden(app, 5000);
    if (existsSync(join(userDir, "plugins", "test_qml_only")))
      throw new Error("test_qml_only is in the user's plugins after the cancel");
  }, own.skip ? { skip: own.skip } : { env: own.repo.env }));
if (own.repo) await own.repo.close();

// B8. A version picked in App Manager's details for an optional package goes
// to Basecamp with the install: the dialog's row for it starts at that
// version, and it is the one fetched. The row used to show the newest while
// the plan held the pick.
let optional = null;
try {
  const dir = mkdtempSync(join(tmpdir(), "basecamp-lifecycle-optional-"));
  const lgxDir = join(dir, "lgx");
  mkdirSync(lgxDir);
  const pkg = (name, file, version, extra = {}) => makeLgx({
    outFile: join(lgxDir, file), name, version, variant: process.env.LGX_VARIANT, ...extra });
  const packages = [pkg("opt_dep", "opt_dep-1.0.0.lgx", "1.0.0"),
                    pkg("opt_dep", "opt_dep-1.1.0.lgx", "1.1.0"),
                    pkg("opt_app", "opt_app.lgx", "1.0.0", { optionalDependencies: ["opt_dep"] })];
  optional = { repo: await serveRepository(join(dir, "repo"), packages) };
} catch (e) {
  optional = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("pick an optional: the version picked in App Manager for an optional package is what installs",
  async ({ app }) => {
    const { repo } = optional;
    const fetched = () => repo.requests.filter((p) => p.endsWith(".lgx"));
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(repo.url)})`);
    await app.evalOn(greeting, `backend.setCurrentActiveSectionIndex(1)`);

    const tile = await app.requireObject("appManager.app.opt_app", APP_MANAGER_WAIT_MS);
    await app.send("callMethod", { objectId: tile.id, method: "clicked" });
    const optDep = () => app.requireObject("appManager.details.package.opt_dep", 30000);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(await optDep(), "row.version"), "1.1.0", "opt_dep's newest, before a pick");
      assertEq(await app.evalOn(await optDep(), "row.selected"), true, "opt_dep, ticked");
    }, { timeout: 30000, interval: 300, description: "opt_dep in the details" });
    await pickVersion(app, await optDep(), "1.0.0");
    await app.waitFor(async () => {
      assertEq(await app.evalOn(await optDep(), "row.version"), "1.0.0", "opt_dep at the version picked");
    }, { timeout: 30000, interval: 300, description: "the pick to show in the details" });

    await clickDetailsInstall(app);
    const gate = await app.requireObject(INSTALL_GATE);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(gate, "visible"), true, `${INSTALL_GATE} visible`);
    }, { timeout: 30000, interval: 300, description: "the install gate to open" });
    await app.waitFor(async () => {
      const picker = await app.requireObject("packageRow.version.opt_dep", 30000);
      assertEq(await app.evalOn(picker, "displayText"), "v.1.0.0", "the gate's opt_dep version");
    }, { timeout: 30000, interval: 300, description: "the gate to show opt_dep at 1.0.0" });
    const confirm = await app.requireObject(`${INSTALL_GATE}.confirm`);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(confirm, "enabled"), true, "Install enabled");
    }, { timeout: 30000, interval: 300, description: "the gate's trust check" });
    await app.send("callMethod", { objectId: confirm.id, method: "clicked" });
    await waitGateHidden(app, 5000);

    await app.requireObject("sidebar.app.opt_app", 60000);
    await app.waitFor(async () => {
      assertEq(JSON.stringify(fetched().sort()), JSON.stringify(["/opt_app.lgx", "/opt_dep-1.0.0.lgx"]),
               "the install's downloads");
    }, { timeout: 60000, interval: 500, description: "opt_dep to be fetched" });
  }, optional.skip ? { skip: optional.skip } : { env: optional.repo.env }));
if (optional.repo) await optional.repo.close();

// B9. A confirmation let through the queue is planned on what is installed
// when it starts: Package Manager's request to remove a package waits behind
// App Manager installing an app that needs it, and the removal's plan names
// that app. It used to be planned on the graph from before the install.
let fresh = null;
try {
  const dir = mkdtempSync(join(tmpdir(), "basecamp-lifecycle-fresh-"));
  const lgxDir = join(dir, "lgx");
  mkdirSync(lgxDir);
  const pkg = (name, extra = {}) => makeLgx({
    outFile: join(lgxDir, `${name}.lgx`), name, version: "1.0.0", variant: process.env.LGX_VARIANT, ...extra });
  fresh = { repo: await serveRepository(join(dir, "repo"),
                                        [pkg("dep_lib"), pkg("dep_app", { dependencies: ["dep_lib"] })]) };
} catch (e) {
  fresh = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("fresh plan: a removal let through after an install is planned on what it installed",
  async ({ app }) => {
    const { repo } = fresh;
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(repo.url)})`);
    await app.evalOn(greeting, `backend.setCurrentActiveSectionIndex(1)`);

    const askToInstall = async (name) => {
      const tile = await app.requireObject(`appManager.app.${name}`, APP_MANAGER_WAIT_MS);
      await app.send("callMethod", { objectId: tile.id, method: "clicked" });
      await clickDetailsInstall(app);
      const gate = await app.requireObject(INSTALL_GATE);
      await app.waitFor(async () => {
        assertEq(await app.evalOn(gate, "visible ? moduleName : ''"), name, "the install gate");
      }, { timeout: 30000, interval: 300, description: `the install gate for ${name}` });
    };
    const confirm = async () => {
      const button = await app.requireObject(`${INSTALL_GATE}.confirm`);
      await app.waitFor(async () => {
        assertEq(await app.evalOn(button, "enabled"), true, "Install enabled");
      }, { timeout: 30000, interval: 300, description: "the gate's trust check" });
      await app.send("callMethod", { objectId: button.id, method: "clicked" });
    };

    await askToInstall("dep_lib");
    await confirm();
    await waitGateHidden(app, 5000);
    await app.requireObject("sidebar.app.dep_lib", 60000);

    await askToInstall("dep_app");
    // Package Manager asks to remove dep_lib; it waits behind the install.
    await openPackageManager(app);
    const store = await app.requireObject("pmui.BackendStore", APP_MANAGER_WAIT_MS);
    await app.evalOn(store, '(logos.request("basecamp.packages.confirm_uninstall", ' +
                            '{ names: ["dep_lib"] }, function () {}), "sent")');
    await confirm();

    const dialog = await app.requireObject("uninstallDialog", 60000);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(dialog, "visible"), true, "uninstallDialog visible");
      assertEq(await app.evalOn(dialog, "plan.loading === true"), false, "the plan, read");
    }, { timeout: 60000, interval: 300, description: "the removal's confirmation" });
    assertEq(await app.evalOn(dialog, "JSON.stringify(plan.batch)"), JSON.stringify(["dep_lib"]),
             "what it removes");
    assertEq(await app.evalOn(dialog, "JSON.stringify(plan.dependents.map(d => d.name))"),
             JSON.stringify(["dep_app"]), "what needs it");
    const cancel = await app.requireObject("uninstallDialog.cancelButton");
    await app.send("callMethod", { objectId: cancel.id, method: "clicked" });
  }, fresh.skip ? { skip: fresh.skip } : { env: fresh.repo.env }));
if (fresh.repo) await fresh.repo.close();

// B10. An install that replaces a package that is open: the gate names it
// as running, and approving unloads it before the installer swaps the files.
// The extracted gate used to replace the package under it. (What runs on it
// stops too: replace_impact_test; an app here can't run on another app.)
let swap = null;
try {
  const dir = mkdtempSync(join(tmpdir(), "basecamp-lifecycle-swap-"));
  const lgxDir = join(dir, "lgx");
  mkdirSync(lgxDir);
  const pkg = (name, file, version, extra = {}) => makeLgx({
    outFile: join(lgxDir, file), name, version, variant: process.env.LGX_VARIANT, ...extra });
  swap = { repo: await serveRepository(join(dir, "repo"), [
    pkg("shared_dep", "shared_dep-1.0.0.lgx", "1.0.0"),
    pkg("shared_dep", "shared_dep-2.0.0.lgx", "2.0.0"),
    pkg("uses_shared", "uses_shared.lgx", "1.0.0", { dependencies: ["shared_dep"] }),
    pkg("needs_new", "needs_new.lgx", "1.0.0",
        { dependencies: [{ name: "shared_dep", version: ">=2.0.0" }] }),
  ]) };
} catch (e) {
  swap = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("swap: an install that replaces an open package unloads it first",
  async ({ app }) => {
    const { repo } = swap;
    const fetched = () => repo.requests.filter((p) => p.endsWith(".lgx"));
    const greeting = await app.requireObject("welcomePage.greeting");
    const isLoaded = (name) => app.evalOn(greeting,
      `JSON.stringify(backend.launcherApps.filter(a => a.name === ${JSON.stringify(name)}).map(a => a.isLoaded))`);
    const gate = await app.requireObject(INSTALL_GATE);
    const confirm = async (name) => {
      await app.waitFor(async () => {
        assertEq(await app.evalOn(gate, "visible ? moduleName : ''"), name, "the install gate");
      }, { timeout: 30000, interval: 300, description: `the install gate for ${name}` });
      const button = await app.requireObject(`${INSTALL_GATE}.confirm`);
      await app.waitFor(async () => {
        assertEq(await app.evalOn(button, "enabled"), true, "Install enabled");
      }, { timeout: 30000, interval: 300, description: "the gate's trust check" });
      await app.send("callMethod", { objectId: button.id, method: "clicked" });
    };
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(repo.url)})`);
    await app.evalOn(greeting, `backend.setCurrentActiveSectionIndex(1)`);

    // uses_shared, on shared_dep 1.0.0 as picked in its details.
    const usesTile = await app.requireObject("appManager.app.uses_shared", APP_MANAGER_WAIT_MS);
    await app.send("callMethod", { objectId: usesTile.id, method: "clicked" });
    const dep = () => app.requireObject("appManager.details.package.shared_dep", 30000);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(await dep(), "row.version"), "2.0.0", "shared_dep's newest, before a pick");
    }, { timeout: 30000, interval: 300, description: "shared_dep in the details" });
    await pickVersion(app, await dep(), "1.0.0");
    await app.waitFor(async () => {
      assertEq(await app.evalOn(await dep(), "row.version"), "1.0.0", "shared_dep at the version picked");
    }, { timeout: 30000, interval: 300, description: "the pick to show" });
    await clickDetailsInstall(app);
    await confirm("uses_shared");
    await waitGateHidden(app, 5000);
    await app.requireObject("sidebar.app.uses_shared", 60000);

    await app.evalOn(greeting, '(backend.loadUiModule("shared_dep"), "loaded")');
    await app.waitFor(async () => {
      assertEq(await isLoaded("shared_dep"), "[true]", "shared_dep open");
    }, { timeout: 30000, interval: 300, description: "shared_dep to open" });

    // needs_new takes shared_dep to 2.0.0 under it.
    await app.evalOn(greeting, `backend.setCurrentActiveSectionIndex(1)`);
    const newTile = await app.requireObject("appManager.app.needs_new", APP_MANAGER_WAIT_MS);
    await app.send("callMethod", { objectId: newTile.id, method: "clicked" });
    await clickDetailsInstall(app);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(gate, "visible ? moduleName : ''"), "needs_new", "the install gate");
    }, { timeout: 30000, interval: 300, description: "the install gate for needs_new" });
    await app.waitFor(async () => {
      // Every dialog has a list by that name: the gate's.
      const running = await app.evalOn(gate, `(() => {
        const find = (node) => {
          if (!node) return null;
          if (node.objectName === "confirmationDialog.running") return node;
          const kids = node.children;
          for (let i = 0; kids && i < kids.length; i += 1) {
            const found = find(kids[i]);
            if (found) return found;
          }
          return null;
        };
        const list = find(contentItem);
        return JSON.stringify(list ? list.model : null);
      })()`);
      assertEq(running, JSON.stringify(["shared_dep"]), "what the gate says stops");
    }, { timeout: 30000, interval: 300, description: "the gate to name shared_dep" });
    await confirm("needs_new");
    await waitGateHidden(app, 5000);
    await app.requireObject("sidebar.app.needs_new", 60000);
    assertEq(await isLoaded("shared_dep"), "[false]", "shared_dep, unloaded for the swap");
    if (!fetched().includes("/shared_dep-2.0.0.lgx"))
      throw new Error(`shared_dep 2.0.0 was not fetched: ${JSON.stringify(fetched())}`);
  }, swap.skip ? { skip: swap.skip } : { env: swap.repo.env }));
if (swap.repo) await swap.repo.close();

// B11. Two repositories list the app: one picked from the second (a welcome
// page tile's openApp, with its repository) is the second's in App Manager,
// and installs from there. Basecamp used to drop the repository, and App
// Manager showed the app from the first.
let twoRepos = null;
try {
  const first = await serveRepository(mkdtempSync(join(tmpdir(), "basecamp-lifecycle-first-")),
                                      [FIXTURES.A_0_1_1], { name: "first-repo", displayName: "First Repo" });
  const second = await serveRepository(mkdtempSync(join(tmpdir(), "basecamp-lifecycle-second-")),
                                       [FIXTURES.A_0_1_1], { name: "second-repo", displayName: "Second Repo" });
  // One trust file for both certificates.
  const trust = join(mkdtempSync(join(tmpdir(), "basecamp-lifecycle-trust-")), "trust.pem");
  writeFileSync(trust, readFileSync(first.env.SSL_CERT_FILE, "utf-8") + "\n"
                       + readFileSync(second.env.SSL_CERT_FILE, "utf-8"));
  twoRepos = { first, second, env: { SSL_CERT_FILE: trust } };
} catch (e) {
  twoRepos = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("two repositories: an app picked from the second installs from it",
  async ({ app }) => {
    const { first, second } = twoRepos;
    const fetched = (repo) => repo.requests.filter((p) => p.endsWith(".lgx"));
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(first.url)})`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(second.url)})`);
    const urls = JSON.stringify([first.url, second.url]);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(greeting, `${urls}.every(u => backend.repositories.some(r => r.url === u))`),
               true, "both repositories added");
    }, { timeout: 30000, interval: 300, description: "both repositories" });
    await app.evalOn(greeting, "backend.setCurrentActiveSectionIndex(1)");
    await app.requireObject("appManager.app.test_qml_only", APP_MANAGER_WAIT_MS);
    await app.evalOn(greeting, "backend.setCurrentActiveSectionIndex(0)");

    await app.evalOn(greeting, `(backend.openApp("test_qml_only", ${JSON.stringify(second.url)}, true), "sent")`);
    const gate = await app.requireObject(INSTALL_GATE);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(gate, "visible ? moduleName : ''"), "test_qml_only", "the install gate");
    }, { timeout: 30000, interval: 300, description: "the install gate for test_qml_only" });
    const confirm = await app.requireObject(`${INSTALL_GATE}.confirm`);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(confirm, "enabled"), true, "Install enabled");
    }, { timeout: 30000, interval: 300, description: "the gate's trust check" });
    await app.send("callMethod", { objectId: confirm.id, method: "clicked" });
    await waitGateHidden(app, 5000);
    await app.requireObject("sidebar.app.test_qml_only", 60000);
    assertEq(JSON.stringify(fetched(first)), "[]", "what the first repository served");
    assertEq(fetched(second).length, 1, `what the second repository served (${JSON.stringify(fetched(second))})`);
  }, twoRepos.skip ? { skip: twoRepos.skip } : { env: twoRepos.env }));
if (twoRepos.first) {
  await twoRepos.first.close();
  await twoRepos.second.close();
}

// B12 and B13: an approved App Manager install that ends without its change
// on disk ends it all the same, and a Package Manager confirmation waiting
// behind it shows at once. It used to wait two minutes of nothing.
//
// Each serves `keep` and `then` from one repository (shaped by `serve`),
// installs `keep`, has App Manager install `then` (which never lands,
// `afterApproval` doing its part), and then Package Manager asks to remove
// `keep`.
async function endedChangeLetsTheNextThrough(app, userDir, { repo, keep, then, afterApproval = async () => {} }) {
  const greeting = await app.requireObject("welcomePage.greeting");
  await app.evalOn(greeting, `backend.setDownloadSource("http")`);
  await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(repo.url)})`);
  await app.evalOn(greeting, `backend.setCurrentActiveSectionIndex(1)`);
  const gate = await app.requireObject(INSTALL_GATE);
  const install = async (name) => {
    const tile = await app.requireObject(`appManager.app.${name}`, APP_MANAGER_WAIT_MS);
    await app.send("callMethod", { objectId: tile.id, method: "clicked" });
    await clickDetailsInstall(app);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(gate, "visible ? moduleName : ''"), name, "the install gate");
    }, { timeout: 30000, interval: 300, description: `the install gate for ${name}` });
    const button = await app.requireObject(`${INSTALL_GATE}.confirm`);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(button, "enabled"), true, "Install enabled");
    }, { timeout: 30000, interval: 300, description: "the gate's trust check" });
    await app.send("callMethod", { objectId: button.id, method: "clicked" });
    await waitGateHidden(app, 5000);
  };

  await install(keep);
  await app.requireObject(`sidebar.app.${keep}`, 60000);
  await install(then);
  await afterApproval();

  await openPackageManager(app);
  const store = await app.requireObject("pmui.BackendStore", APP_MANAGER_WAIT_MS);
  await app.evalOn(store, '(logos.request("basecamp.packages.confirm_uninstall", ' +
                          `{ names: [${JSON.stringify(keep)}] }, function () {}), "sent")`);
  // Well inside the two minutes.
  const dialog = await app.requireObject("uninstallDialog", 20000);
  await app.waitFor(async () => {
    assertEq(await app.evalOn(dialog, "visible"), true, "uninstallDialog visible");
  }, { timeout: 20000, interval: 300, description: "the removal's confirmation" });
  if (existsSync(join(userDir, "plugins", then)))
    throw new Error(`${then} is installed after all`);
  const cancel = await app.requireObject("uninstallDialog.cancelButton");
  await app.send("callMethod", { objectId: cancel.id, method: "clicked" });
}

function twoAppsRepo(prefix, opts = {}) {
  const dir = mkdtempSync(join(tmpdir(), `basecamp-lifecycle-${prefix}-`));
  const lgxDir = join(dir, "lgx");
  mkdirSync(lgxDir);
  const pkg = (name) => makeLgx({
    outFile: join(lgxDir, `${name}.lgx`), name, version: "1.0.0", variant: process.env.LGX_VARIANT });
  return { dir, packages: [pkg(opts.keep), pkg(opts.then)] };
}

// B12. Its download fails: App Manager says the install ended (finished).
let failing = null;
try {
  const { dir, packages } = twoAppsRepo("failing", { keep: "kept_app", then: "lost_app" });
  failing = { repo: await serveRepository(join(dir, "repo"), packages) };
  // Listed, but its file is gone.
  rmSync(join(dir, "repo", "www", "lost_app.lgx"));
} catch (e) {
  failing = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("failed install: App Manager saying so lets the next confirmation through",
  async ({ app, userDir }) => {
    await endedChangeLetsTheNextThrough(app, userDir, { repo: failing.repo, keep: "kept_app", then: "lost_app" });
  }, failing.skip ? { skip: failing.skip } : { env: failing.repo.env }));
if (failing.repo) await failing.repo.close();

// B13. Its download stays on its way, and the person closes App Manager.
let closing = null;
try {
  const { dir, packages } = twoAppsRepo("closing", { keep: "kept_app", then: "slow_app" });
  closing = { repo: await serveRepository(join(dir, "repo"), packages, { stall: ["/slow_app.lgx"] }) };
} catch (e) {
  closing = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("closed mid-install: closing App Manager lets the next confirmation through",
  async ({ app, userDir }) => {
    const { repo } = closing;
    await endedChangeLetsTheNextThrough(app, userDir, {
      repo, keep: "kept_app", then: "slow_app",
      afterApproval: async () => {
        await app.waitFor(async () => {
          assertEq(repo.requests.includes("/slow_app.lgx"), true, "slow_app's download asked for");
        }, { timeout: 30000, interval: 300, description: "slow_app's download" });
        const greeting = await app.requireObject("welcomePage.greeting");
        await app.evalOn(greeting, '(backend.unloadUiModule("app_manager_ui"), "closing")');
      },
    });
  }, closing.skip ? { skip: closing.skip } : { env: closing.repo.env }));
if (closing.repo) await closing.repo.close();

// B14. A package that isn't an app, from the Welcome page's Packages row:
// Package Manager shows it (packages.install), and the person's Install there
// asks for the gate, as before App Manager left Basecamp. It went to App
// Manager, which shows only apps, and nothing came up.
let packageRow = null;
try {
  const dir = mkdtempSync(join(tmpdir(), "basecamp-lifecycle-package-row-"));
  const lgxDir = join(dir, "lgx");
  mkdirSync(lgxDir);
  packageRow = { repo: await serveRepository(join(dir, "repo"), [makeLgx({
    outFile: join(lgxDir, "row_module.lgx"), name: "row_module", version: "1.0.0",
    type: "core", variant: process.env.LGX_VARIANT })]) };
} catch (e) {
  packageRow = { skip: `no local HTTPS repository: ${e.message}` };
}
results.push(await runTest("package row: Package Manager shows a package from the Welcome page",
  async ({ app }) => {
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(packageRow.repo.url)})`);

    // What the row does for a package that isn't installed.
    await app.evalOn(greeting, '(root.packageInstallRequested("row_module"), "sent")');
    await app.waitFor(async () => {
      assertEq(await app.evalOn(greeting, "backend.currentActiveSectionIndex"), 2, "the section in front");
      const header = await app.requireObject("pmui.header", 10000);
      assertEq(await app.evalOn(header, "searchText"), "row_module", "Package Manager's search");
    }, { timeout: 30000, interval: 300, description: "Package Manager to show row_module" });
  }, packageRow.skip ? { skip: packageRow.skip } : { env: packageRow.repo.env }));

// B15. An install offer for a package that isn't an app (beginIntentInstall,
// which openApp routes by the package's type): Package Manager shows it too.
results.push(await runTest("install offer: Package Manager shows a package an offer names",
  async ({ app }) => {
    const greeting = await app.requireObject("welcomePage.greeting");
    await app.evalOn(greeting, `backend.setDownloadSource("http")`);
    await app.evalOn(greeting, `backend.addRepository(${JSON.stringify(packageRow.repo.url)})`);
    // Something else in its search first, so the offer is what fills it.
    await openPackageManager(app);
    const header = await app.requireObject("pmui.header", APP_MANAGER_WAIT_MS);
    await app.evalOn(header, "openSearch('zzz')");
    await app.evalOn(greeting, "backend.setCurrentActiveSectionIndex(0)");

    await app.evalOn(greeting, '(backend.beginIntentInstall("row_module"), "sent")');
    await app.waitFor(async () => {
      assertEq(await app.evalOn(greeting, "backend.currentActiveSectionIndex"), 2, "the section in front");
      assertEq(await app.evalOn(header, "searchText"), "row_module", "Package Manager's search");
    }, { timeout: 30000, interval: 300, description: "Package Manager to show row_module" });
  }, packageRow.skip ? { skip: packageRow.skip } : { env: packageRow.repo.env }));
if (packageRow.repo) await packageRow.repo.close();

const ran = results.filter((r) => r !== null);
const passed = ran.filter((r) => r === "pass").length;
const failed = ran.filter((r) => r === "fail").length;
const skipped = ran.filter((r) => r === "skip").length;
console.log(`\n${passed} passed, ${failed} failed, ${skipped} skipped`);
console.log(`Total elapsed: ${((Date.now() - suiteStart) / 1000).toFixed(1)}s`);
process.exit(failed > 0 ? 1 : 0);
