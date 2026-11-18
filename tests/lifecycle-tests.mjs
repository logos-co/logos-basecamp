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
const INSTALL_ERROR = "confirmationDialog.installError";

// What PackageCoordinator::beginPackageConfirmation logs when a confirm_*
// intent arrives while a dialog already owns the screen.
const REFUSAL_LOG = "confirmation in flight, refusing";
// What UIPluginManager::onPmuiInstallProgress logs when PMUI reports a failed
// install over its progress signal (the same report opens INSTALL_ERROR).
const PMUI_FAILURE_LOG = "package_manager_ui install failure for";

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

// PMU's installLocalPackage with nothing waited on: for a test that expects
// the gate to stay open, or another dialog, or a refusal. Returns the moment
// the call had been made on the Qt side.
async function installLocalPackage(app, store, lgxPath) {
  await app.send("callMethod", {
    objectId: store.id, method: "installLocalPackage", args: [`file://${resolve(lgxPath)}`],
  });
  return Date.now();
}

// Where PMUI's refusal of a request can be read from here, best first. The
// plan names PMUI's own notice `pmui.messageNotice`; package_manager_ui is a
// flake input (not in this tree), so whether its notice carries that
// objectName could not be checked at the pinned revision from this checkout.
// It is probed at run time instead: a shown message there wins; otherwise
// the next two witnesses stand in — Basecamp's own "Install Failed" notice,
// raised when PMUI reports the failed install over its progress signal
// (UIPluginManager::onPmuiInstallProgress), and the log line that same
// report leaves. Returns null when none has anything yet; `hook: true` when
// the PMUI object exists but shows nothing and no other witness has anything
// either (the hook exists, so a silent refusal is a failure, not a gap).
async function pmuiErrorReport(app, log) {
  const notice = await app.findByObjectName("pmui.messageNotice");
  if (notice) {
    const state = JSON.parse(await app.evalOn(notice, `JSON.stringify({
      visible: typeof visible === "boolean" ? visible : null,
      shown: typeof shown === "boolean" ? shown : null,
      title: typeof title === "string" ? title : "",
      text: typeof text === "string" ? text : "",
      message: typeof message === "string" ? message : "",
    })`));
    const words = [state.title, state.text, state.message].filter((s) => s.length > 0);
    const hidden = state.visible === false || state.shown === false;
    if (words.length > 0 && !hidden) return { via: "pmui.messageNotice", text: words.join(" — ") };
  }
  const dialog = await app.findByObjectName(INSTALL_ERROR);
  if (dialog && (await app.evalOn(dialog, "visible")) === true) {
    const name = await app.evalOn(dialog, "moduleName");
    const message = await app.evalOn(dialog, "errorMessage");
    return { via: INSTALL_ERROR, text: `${name}: ${message}` };
  }
  const line = log().split("\n").find((l) => l.includes(PMUI_FAILURE_LOG));
  if (line) return { via: "app log", text: line.trim() };
  return notice ? { via: "pmui.messageNotice", text: "", hook: true } : null;
}

// Closes Basecamp's "Install Failed" notice if it is open.
async function dismissInstallError(app) {
  const dialog = await app.findByObjectName(INSTALL_ERROR);
  if (!dialog || (await app.evalOn(dialog, "visible")) !== true) return;
  await app.click(`${INSTALL_ERROR}.confirm`);
  await app.waitFor(async () => {
    assertEq(await app.evalOn(dialog, "visible"), false, `${INSTALL_ERROR} visible`);
  }, { timeout: 5000, interval: 100, description: "the Install Failed notice to close" });
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

// B2. Two confirm_install requests back-to-back. Only one dialog may own the
// screen, so PackageCoordinator::beginPackageConfirmation refuses the second
// — with an answer (errFailed, logged as "confirmation in flight, refusing"),
// never by repointing the open gate at the newcomer (the user would be
// consenting to a package they never read about) and never by dropping the
// dispatch (the requester would wait out the broker's full deadline). The
// refusal releases nothing it did not take: the first gate still answers for
// its own package, and a later request for the refused one opens a fresh gate.
results.push(await runTest("install: two confirmations in flight — the second is refused with an answer, never clobbered or dropped",
  async ({ app, userDir, log, note }) => {
    await openPackageManager(app);
    const store = await app.requireObject("pmui.BackendStore");
    const gate = await app.requireObject(INSTALL_GATE);
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible before any install`);
    const pluginDirA = join(userDir, "plugins", "test_qml_only");
    const pluginDirB = join(userDir, "plugins", "test_qml_only_b");
    const gateNames = async () => ({
      name: await app.evalOn(gate, "moduleName"),
      ...(await gateTexts(app, gate)),
    });

    // Two raw requests, A then B, back-to-back: installViaPmu cannot fire the
    // second, it blocks on the gate the first one opens.
    const firstDone = await installLocalPackage(app, store, FIXTURES.A_0_1_1);
    const secondDone = await installLocalPackage(app, store, FIXTURES.B_0_1_1);
    const gapMs = secondDone - firstDone;
    note(`second installLocalPackage made ${gapMs} ms after the first`);

    // Exactly one gate, and it is A's: moduleName (what confirm answers for)
    // and the body (what the user reads) agree on test_qml_only.
    await app.waitFor(async () => {
      assertEq(await app.evalOn(gate, "visible"), true, `${INSTALL_GATE} visible`);
    }, { timeout: 30000, interval: 200, description: "the install gate to open" });
    const A_BODY_RE = /^Install '(Test QML Only|test_qml_only)' v0\.1\.1\b/;
    const assertGateIsA = async (when) => {
      const { name, title, body } = await gateNames();
      assertEq(title, "Install Package?", `install gate title ${when}`);
      assertEq(name, "test_qml_only", `install gate moduleName ${when}`);
      if (!A_BODY_RE.test(body) || /test_qml_only_b|Test QML Only B/.test(body)) {
        throw new Error(`install gate body ${when} does not name A alone: ${JSON.stringify(body)}`);
      }
      return body;
    };
    note(`gate body: ${await assertGateIsA("on open")}`);
    const visibleGates = [];
    for (const m of await app.findByProperty("objectName", INSTALL_GATE)) {
      if ((await app.evalOn(m, "visible")) === true) visibleGates.push(m);
    }
    assertEq(visibleGates.length, 1, "visible install gates");
    const upgradeGate = await app.requireObject("confirmationDialog.upgradeCascade");
    assertEq(await app.evalOn(upgradeGate, "visible"), false, "confirmationDialog.upgradeCascade visible");

    // Before the gate is answered: B is refused, and refused with an answer.
    await app.waitFor(async () => {
      if (!log().includes(REFUSAL_LOG)) throw new Error(`app log has no "${REFUSAL_LOG}"`);
    }, { timeout: 5000, interval: 100, description: "the second confirmation to be refused" });
    let report = null;
    await app.waitFor(async () => {
      report = await pmuiErrorReport(app, log);
      if (!report || report.text.length === 0) {
        throw new Error("no error reported for B yet" + (report?.hook ? " (pmui.messageNotice is empty)" : ""));
      }
    }, { timeout: 5000, interval: 200, description: "PMUI to report an error for B" });
    if (report && report.text.length > 0) {
      note(`B refused, reported via ${report.via}: ${report.text}`);
      // By its raw name or its display name (the fixture's "Test QML Only B"),
      // whichever the witness renders — but it has to be about B.
      if (!/test_qml_only_b|Test QML Only B/.test(report.text)) {
        throw new Error(`the error reported via ${report.via} does not name test_qml_only_b: ${report.text}`);
      }
    }
    // The refusal must not have touched A's gate.
    assertEq(await app.evalOn(gate, "visible"), true, `${INSTALL_GATE} visible after the refusal`);
    await assertGateIsA("after the refusal");
    // Basecamp's own notice, if the refusal surfaced there, must not sit over
    // the gate when it is answered.
    await dismissInstallError(app);

    // Confirm A's gate — once the signer check has enabled the button — and
    // look one last time at what is being answered.
    const confirmButton = await app.requireObject(`${INSTALL_GATE}.confirm`);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(confirmButton, "enabled"), true, `${INSTALL_GATE}.confirm enabled`);
    }, { timeout: 30000, interval: 300, description: "the install gate's confirm to enable" });
    await assertGateIsA("when confirmed");
    await app.send("callMethod", { objectId: confirmButton.id, method: "clicked" });
    await waitGateHidden(app, 5000);

    // A installs. B does not, and no gate reopens for it: 10 s in which the
    // install gate stays hidden and B's tile never appears.
    await app.requireObject("sidebar.app.test_qml_only", 60000);
    if (!existsSync(join(pluginDirA, "manifest.json"))) {
      throw new Error(`${pluginDirA}/manifest.json missing after confirming A`);
    }
    const watchEnd = Date.now() + 10000;
    while (Date.now() < watchEnd) {
      if ((await app.evalOn(gate, "visible")) === true) {
        const { name, body } = await gateNames();
        throw new Error(`a second install gate opened after A's was answered: ` +
                        `moduleName=${JSON.stringify(name)} body=${JSON.stringify(body)}`);
      }
      if (await app.findByObjectName("sidebar.app.test_qml_only_b")) {
        throw new Error("sidebar.app.test_qml_only_b appeared: the refused request installed B");
      }
      await sleep(400);
    }
    if (existsSync(pluginDirB)) throw new Error(`${pluginDirB} exists: the refused request installed B`);
    // A late "Install Failed" notice for B is PMUI's report, not a gate.
    await dismissInstallError(app);

    // The slot was released with the answer: asking for B again opens a fresh
    // gate that names B, and confirming it installs B.
    const B_BODY_RE = /^Install '(Test QML Only B|test_qml_only_b)' v0\.1\.1\b/;
    await installViaPmu(app, FIXTURES.B_0_1_1, {
      confirm: true,
      whileOpen: async (open) => {
        const { title, body } = await gateTexts(app, open);
        assertEq(title, "Install Package?", "install gate title for B");
        assertEq(await app.evalOn(open, "moduleName"), "test_qml_only_b", "install gate moduleName for B");
        if (!B_BODY_RE.test(body)) {
          throw new Error(`install gate body for B ${JSON.stringify(body)} does not start ` +
                          `"Install 'test_qml_only_b' v0.1.1"`);
        }
        note(`fresh gate body: ${body}`);
      },
    });
    await waitGateHidden(app, 5000);
    await app.requireObject("sidebar.app.test_qml_only_b", 60000);
    if (!existsSync(join(pluginDirB, "manifest.json"))) {
      throw new Error(`${pluginDirB}/manifest.json missing after confirming B`);
    }
    if (!(await app.findByObjectName("sidebar.app.test_qml_only"))) {
      throw new Error("sidebar.app.test_qml_only vanished while B installed");
    }
  }));

const ran = results.filter((r) => r !== null);
const passed = ran.filter((r) => r === "pass").length;
const failed = ran.filter((r) => r === "fail").length;
const skipped = ran.filter((r) => r === "skip").length;
console.log(`\n${passed} passed, ${failed} failed, ${skipped} skipped`);
console.log(`Total elapsed: ${((Date.now() - suiteStart) / 1000).toFixed(1)}s`);
process.exit(failed > 0 ? 1 : 0);
