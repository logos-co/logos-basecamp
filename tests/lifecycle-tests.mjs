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
import { existsSync, mkdtempSync } from "node:fs";
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
import { makeLifecycleFixtures } from "./fixtures/lgx.mjs";

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
// else PASS. opts: { xfail } — loud on unexpected pass; { skip } — not run.
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
  const child = spawnBasecamp(APP_BIN, userDir);
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

// Title and body of an OPEN gate, read from its contentItem subtree: the
// title is the first rendered text (the header row), the body the LogosText
// named confirmationDialog.body.
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
    walk(contentItem);
    return JSON.stringify(out);
  })()`));
  const body = nodes.find((n) => n.objectName === "confirmationDialog.body");
  const title = nodes.find((n) => n.text.length > 0 && n !== body);
  return { title: title ? title.text : "", body: body ? body.text : "", texts: nodes.map((n) => n.text) };
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

const ran = results.filter((r) => r !== null);
const passed = ran.filter((r) => r === "pass").length;
const failed = ran.filter((r) => r === "fail").length;
const skipped = ran.filter((r) => r === "skip").length;
console.log(`\n${passed} passed, ${failed} failed, ${skipped} skipped`);
console.log(`Total elapsed: ${((Date.now() - suiteStart) / 1000).toFixed(1)}s`);
process.exit(failed > 0 ? 1 : 0);
