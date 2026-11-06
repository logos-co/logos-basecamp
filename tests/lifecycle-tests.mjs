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
import { existsSync, mkdtempSync, readdirSync, readFileSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import {
  assertResponsive,
  expectAbsent,
  findByObjectName,
  installViaPmu,
  makeUserDir,
  pollProperty,
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
    if (await app.findByObjectName("welcomePage.greeting")) return;
    if (await app.findByObjectName("onboardingView")) { onboarding = true; return; }
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

// The witnesses that only a FAILED install leaves: Basecamp's "Install Failed"
// notice and the log line behind it. Not pmui.messageNotice, whose text could
// as well be a status ("Installing…") or a success as an error. For a test
// that must not mistake a successful install for a reported failure.
async function installFailureWitness(app, log) {
  const dialog = await app.findByObjectName(INSTALL_ERROR);
  if (dialog && (await app.evalOn(dialog, "visible")) === true) {
    const name = await app.evalOn(dialog, "moduleName");
    const message = await app.evalOn(dialog, "errorMessage");
    return { via: INSTALL_ERROR, text: `${name}: ${message}` };
  }
  const line = log().split("\n").find((l) => l.includes(PMUI_FAILURE_LOG));
  if (line) return { via: "app log", text: line.trim() };
  return null;
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
    if (gapMs > 200) {
      throw new Error(`the two installLocalPackage calls were ${gapMs} ms apart, want under 200 ms`);
    }

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
    }, { timeout: 5000, interval: 200, description: "PMUI to report an error for B" }).catch((e) => {
      // The hook exists and stays empty: the refusal went unreported, a
      // failure. No hook at all: the refusal is still proven by the log line
      // above and by no second gate ever opening (below); the missing
      // automation hook is a finding, not something to paper over here.
      if (report?.hook) throw e;
      note("PMUI's error for B is not readable from the shell (no pmui.messageNotice, " +
           "no Install Failed notice, no install-failure log line); the refusal is " +
           "asserted through Basecamp's log line and the no-second-gate watch");
    });
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

// B3. An install gate nobody answers. PMUI's confirm_install is an intent the
// shell itself provides, dispatched with no chooser, so the broker's
// Dispatched phase bounds it: 20 s (m_responseTimeoutMs) when the provider
// had no handler, the 10-minute leak backstop (kChooserBackstopMs) when it
// had one. MainUIBackend's shell endpoint reports one receiver whenever
// PackageCoordinator::beginPackageConfirmation takes the request, so a gate
// that opened is under the backstop: 30 s of silence — past the 20 s deadline
// on purpose — must leave it open, and the late answer still installs. Were
// the gate gone at 30 s, the message says whether PMUI was told (the 20 s
// deadline applied: a product finding, and this test would then assert that)
// or nothing was (a silent drop, a failure either way). A gate still open
// over a request the broker has already ended would fail the same way: PMUI
// reports the failure before the gate is answered, and that is checked at
// the 30 s mark too, so it does not surface as a bare "A never installed".
results.push(await runTest("install: gate left unanswered stays open and still works when answered",
  async ({ app, userDir, log, note }) => {
    await openPackageManager(app);
    const store = await app.requireObject("pmui.BackendStore");
    const gate = await app.requireObject(INSTALL_GATE);
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible before any install`);
    const pluginDir = join(userDir, "plugins", "test_qml_only");

    const requested = await installLocalPackage(app, store, FIXTURES.A_0_1_1);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(gate, "visible"), true, `${INSTALL_GATE} visible`);
    }, { timeout: 30000, interval: 200, description: "the install gate to open" });
    const openedAfterMs = Date.now() - requested;
    assertEq(await app.evalOn(gate, "moduleName"), "test_qml_only", "install gate moduleName");
    note(`gate opened ${openedAfterMs} ms after installLocalPackage`);

    // The observation window: a fixed 30 s with no click, `visible` sampled
    // throughout so a gate that closed and reopened would show as well.
    const UNANSWERED_MS = 30000;
    const samples = await pollProperty(app, gate.id, "visible", UNANSWERED_MS, { intervalMs: 500 });
    const stillOpen = await app.evalOn(gate, "visible");
    const flips = samples.slice(1);
    if (stillOpen !== true || flips.length > 0) {
      const report = await pmuiErrorReport(app, log);
      const told = report && report.text.length > 0;
      const timedOut = told && /timed?[ _-]?out/i.test(report.text);
      const history = samples.map((s) => `${s.t}ms:${s.value}`).join(" ");
      throw new Error(
        `${INSTALL_GATE} did not stay open for ${UNANSWERED_MS} ms unanswered ` +
        `(visible=${JSON.stringify(stillOpen)} at the end, samples: ${history}); ` +
        (told
          ? `PMUI reported via ${report.via}: ${report.text}` +
            (timedOut ? " — the 20 s response deadline applied to the shell's own gate"
                      : "")
          : "no error reached PMUI (no pmui.messageNotice text, no Install Failed " +
            "notice, no install-failure log line): a silent drop"));
    }
    // Open, and still a live request: the broker ending the dispatch under
    // the gate (PMUI told of a failure before anyone answered) would make the
    // confirm below answer nothing.
    const early = await installFailureWitness(app, log);
    if (early) {
      throw new Error(`${INSTALL_GATE} is still open after ${UNANSWERED_MS} ms but PMUI ` +
                      `was already told the install failed, via ${early.via}: ${early.text} ` +
                      "— the request ended under the open gate (the 20 s response deadline)");
    }
    note(`gate still visible ${UNANSWERED_MS / 1000} s in with no click (${samples.length} distinct sample(s))`);

    // The late answer. Confirm is enabled once the signer check has run —
    // long since, after 30 s.
    const confirmButton = await app.requireObject(`${INSTALL_GATE}.confirm`);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(confirmButton, "enabled"), true, `${INSTALL_GATE}.confirm enabled`);
    }, { timeout: 5000, interval: 200, description: "the install gate's confirm to be enabled" });
    await app.send("callMethod", { objectId: confirmButton.id, method: "clicked" });
    await waitGateHidden(app, 5000);

    // The answer was delivered and acted on: A installs within 60 s.
    await app.requireObject("sidebar.app.test_qml_only", 60000);
    if (!existsSync(join(pluginDir, "manifest.json"))) {
      throw new Error(`${pluginDir}/manifest.json missing after confirming the late gate`);
    }
    note(`A installed ${Date.now() - requested} ms after the request`);
    // And nothing reported it as failed along the way.
    const late = await installFailureWitness(app, log);
    if (late) {
      throw new Error(`a failure was reported via ${late.via} although A installed: ${late.text}`);
    }
  }));

// B4. The same file a second time. PMUI inspects it, finds the package
// installed at that very version, and raises confirm_upgrade with mode 2
// (PackageTypes/UpgradeMode: Sidegrade), never confirm_install — so the shell
// draws the upgradeCascade dialog in its reinstall wording
// (ConfirmationDialog.qml: "Reinstall Package?", the body leading "Reinstall
// '<label>' to v…", the button "Reinstall"), and the install gate stays
// closed. Confirming hands PMUI the go-ahead: it removes the installed copy
// and extracts the file again (the body says so), which is why the
// manifest's mtime is recorded and reported but not held still. "No-op" is
// the end state: one plugins/test_qml_only directory, one sidebar tile, and
// the app opens and renders its v0.1.1 payload.
const UPGRADE_CASCADE = "confirmationDialog.upgradeCascade";
const LOAD_FAILED = "confirmationDialog.loadFailed";
// What PackageCoordinator logs on package_manager's uiPluginUninstalled /
// uiPluginFileInstalled events: the two halves of a reinstall, in that order.
const UI_PLUGIN_UNINSTALLED_LOG = "UI plugin uninstalled:";
const UI_PLUGIN_INSTALLED_LOG = "UI plugin file installed:";

results.push(await runTest("install: reinstalling the same version — wording and no-op on disk",
  async ({ app, userDir, log, note }) => {
    await openPackageManager(app);
    const store = await app.requireObject("pmui.BackendStore");
    const gate = await app.requireObject(INSTALL_GATE);
    const cascade = await app.requireObject(UPGRADE_CASCADE);
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible before any install`);
    assertEq(await app.evalOn(cascade, "visible"), false, `${UPGRADE_CASCADE} visible before any install`);
    const TILE = "sidebar.app.test_qml_only";
    const pluginsDir = join(userDir, "plugins");
    const pluginDir = join(pluginsDir, "test_qml_only");
    const manifestPath = join(pluginDir, "manifest.json");
    const countLines = (needle) => log().split("\n").filter((l) => l.includes(needle)).length;
    const fixtureDirs = () => readdirSync(pluginsDir, { withFileTypes: true })
      .filter((e) => e.isDirectory() && e.name.startsWith("test_qml_only"))
      .map((e) => e.name).sort();
    const tileCount = async () => (await app.findByProperty("objectName", TILE)).length;

    // The first install: the plain gate, confirmed.
    await installViaPmu(app, FIXTURES.A_0_1_1, { confirm: true });
    await waitGateHidden(app, 5000);
    await app.requireObject(TILE, 60000);
    await app.waitFor(async () => {
      if (!existsSync(manifestPath)) throw new Error(`${manifestPath} missing`);
    }, { timeout: 10000, interval: 250, description: "the installed manifest.json" });
    const before = statSync(manifestPath);
    note(`manifest.json after the first install: mtime ${before.mtime.toISOString()}, inode ${before.ino}`);
    // The first install's own event line, so the baseline below is taken
    // after it (a run with debug logging off never logs it: bounded, not fatal).
    await app.waitFor(async () => {
      if (countLines(UI_PLUGIN_INSTALLED_LOG) < 1) throw new Error(`no "${UI_PLUGIN_INSTALLED_LOG}" yet`);
    }, { timeout: 10000, interval: 250, description: "the first install's event line" }).catch(() => {});
    const uninstalledBefore = countLines(UI_PLUGIN_UNINSTALLED_LOG);
    const installedBefore = countLines(UI_PLUGIN_INSTALLED_LOG);

    // The same file again. Whichever dialog opens first decides: the install
    // gate showing up for a package already installed at this version is the
    // failure this test exists to catch, so it is sampled on every turn.
    const requested = await installLocalPackage(app, store, FIXTURES.A_0_1_1);
    const openDeadline = Date.now() + 30000;
    for (;;) {
      if ((await app.evalOn(gate, "visible")) === true) {
        const { title, body } = await gateTexts(app, gate);
        throw new Error(`${INSTALL_GATE} opened for the second request (title=${JSON.stringify(title)} ` +
                        `body=${JSON.stringify(body)}); want ${UPGRADE_CASCADE} in reinstall mode`);
      }
      if ((await app.evalOn(cascade, "visible")) === true) break;
      if (Date.now() >= openDeadline) {
        throw new Error(`${UPGRADE_CASCADE} did not open within 30 s of the second installLocalPackage`);
      }
      await sleep(200);
    }
    note(`upgradeCascade opened ${Date.now() - requested} ms after the second installLocalPackage`);

    // The wording, read while the dialog is open (a closed instance keeps
    // stale text). The label is the display name once the package is
    // installed, the raw name otherwise; either is accepted and the one seen
    // is noted. The clause after the version says who signed the file (this
    // fixture: nobody), as on the install gate.
    assertEq(await app.evalOn(cascade, "moduleName"), "test_qml_only", `${UPGRADE_CASCADE} moduleName`);
    assertEq(await app.evalOn(cascade, "upgradeModeKind"), 2, `${UPGRADE_CASCADE} upgradeModeKind (2 = reinstall)`);
    assertEq(await app.evalOn(cascade, "upgradeTargetVersion"), "0.1.1", `${UPGRADE_CASCADE} upgradeTargetVersion`);
    const { title, body, texts } = await gateTexts(app, cascade);
    if (!texts.includes("Reinstall Package?")) {
      throw new Error(`open upgradeCascade renders no "Reinstall Package?" (texts: ${JSON.stringify(texts)})`);
    }
    assertEq(title, "Reinstall Package?", "upgradeCascade title");
    const BODY_RE = /^Reinstall '(Test QML Only|test_qml_only)' to v0\.1\.1([^.]*)\. The current version will be removed first, then the new one downloaded and installed\./;
    const bodyMatch = BODY_RE.exec(body);
    if (!bodyMatch) {
      throw new Error(`upgradeCascade body ${JSON.stringify(body)} does not start ` +
                      `"Reinstall 'Test QML Only' to v0.1.1. The current version will be removed first, …"`);
    }
    note(`dialog body: ${body}`);
    note(`label rendered as ${JSON.stringify(bodyMatch[1])} (${bodyMatch[1] === "Test QML Only" ? "display name" : "raw name"})`);

    // The button says "Reinstall" — with " anyway" appended when the signer
    // check asks the user to go ahead with an unsigned package, the same
    // suffix the install gate's "Install" carries. Never "Upgrade",
    // "Downgrade" or "Install". Enabled once the signer check has run.
    const confirmButton = await app.requireObject(`${UPGRADE_CASCADE}.confirm`);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(confirmButton, "enabled"), true, `${UPGRADE_CASCADE}.confirm enabled`);
    }, { timeout: 30000, interval: 300, description: "the reinstall dialog's confirm to enable" });
    const buttonText = await app.evalOn(confirmButton, "text");
    if (!/^Reinstall( anyway)?$/.test(buttonText)) {
      throw new Error(`${UPGRADE_CASCADE}.confirm reads ${JSON.stringify(buttonText)}, want "Reinstall"`);
    }
    note(`confirm button: ${JSON.stringify(buttonText)}`);
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible while the reinstall dialog is open`);
    await app.send("callMethod", { objectId: confirmButton.id, method: "clicked" });
    await app.waitFor(async () => {
      assertEq(await app.evalOn(cascade, "visible"), false, `${UPGRADE_CASCADE} visible`);
    }, { timeout: 5000, interval: 100, description: "the reinstall dialog to close" });

    // PMUI now removes the installed copy and extracts the file again. Wait
    // for the install half to be reported (package_manager's
    // uiPluginFileInstalled event, which also triggers the metadata rescan),
    // watching for a failure report meanwhile; then let the end state settle
    // — manifest, directory listing and tile count unchanged for 3 s — before
    // judging it. A run without the event line (debug logging off) falls
    // through to the settle alone and says so.
    const SETTLE_MS = 60000;
    const settleDeadline = Date.now() + SETTLE_MS;
    let witnessed = false;
    while (Date.now() < settleDeadline) {
      const failure = await installFailureWitness(app, log);
      if (failure) throw new Error(`the reinstall was reported as failed via ${failure.via}: ${failure.text}`);
      if (countLines(UI_PLUGIN_INSTALLED_LOG) > installedBefore) { witnessed = true; break; }
      await sleep(300);
    }
    if (witnessed) {
      note(`reinstall witnessed in the log ${Date.now() - requested} ms after the request ` +
           `(uninstall event: ${countLines(UI_PLUGIN_UNINSTALLED_LOG) > uninstalledBefore ? "yes" : "no"})`);
    } else {
      note(`no "${UI_PLUGIN_INSTALLED_LOG}" line within ${SETTLE_MS / 1000} s; judging the settled end state`);
    }
    const snapshot = async () => {
      const st = existsSync(manifestPath) ? statSync(manifestPath) : null;
      return JSON.stringify({
        manifest: st ? [st.mtimeMs, st.ino, st.size] : null,
        dirs: fixtureDirs(),
        tiles: await tileCount(),
      });
    };
    let last = await snapshot();
    let stableSince = Date.now();
    for (;;) {
      const failure = await installFailureWitness(app, log);
      if (failure) throw new Error(`the reinstall was reported as failed via ${failure.via}: ${failure.text}`);
      const now = await snapshot();
      if (now !== last) { last = now; stableSince = Date.now(); }
      const state = JSON.parse(now);
      const settled = state.manifest !== null && state.tiles >= 1 && Date.now() - stableSince >= 3000;
      if (settled) break;
      if (Date.now() >= settleDeadline) {
        throw new Error(`the reinstalled package did not settle within ${SETTLE_MS / 1000} s ` +
                        `of the request (last state: ${now})`);
      }
      await sleep(500);
    }

    // On disk: exactly one directory for the package, carrying v0.1.1.
    const dirs = fixtureDirs();
    if (dirs.length !== 1 || dirs[0] !== "test_qml_only") {
      throw new Error(`plugins/test_qml_only* after the reinstall: ${JSON.stringify(dirs)}, want ["test_qml_only"]`);
    }
    const manifest = JSON.parse(readFileSync(manifestPath, "utf-8"));
    assertEq(manifest.name, "test_qml_only", "installed manifest.json name");
    assertEq(manifest.version, "0.1.1", "installed manifest.json version");
    const after = statSync(manifestPath);
    const moved = after.mtimeMs !== before.mtimeMs || after.ino !== before.ino;
    note(`manifest.json after the reinstall: mtime ${after.mtime.toISOString()}, inode ${after.ino} ` +
         `— ${moved ? "rewritten (removed and extracted again)" : "unchanged"}`);
    // A failure reported late is still a failure.
    const late = await installFailureWitness(app, log);
    if (late) throw new Error(`a failure was reported via ${late.via} after the reinstall: ${late.text}`);

    // In the sidebar: exactly one tile, and it opens the app.
    assertEq(await tileCount(), 1, `${TILE} instances after the reinstall`);
    const workspace = await app.requireObject("workspace");
    // The evaluate anchor with `backend` in scope that survives dock churn
    // (ui-tests.mjs, waitForVisibleApp); the workspace itself is a C++ widget
    // and only its scalar dockCount is read there.
    const greeting = await app.requireObject("welcomePage.greeting");
    const visibleApp = () => app.evalOn(greeting, "backend.currentVisibleApp");
    assertEq(await app.evalOn(workspace, "dockCount"), 0, "workspace.dockCount before opening the app");
    await app.click(TILE);
    const dockDeadline = Date.now() + 30000;
    for (;;) {
      const loadFailed = await app.findByObjectName(LOAD_FAILED);
      if (loadFailed && (await app.evalOn(loadFailed, "visible")) === true) {
        throw new Error(`the reinstalled app failed to load: ${await app.evalOn(loadFailed, "errorMessage")}`);
      }
      if ((await app.evalOn(workspace, "dockCount")) === 1) break;
      if (Date.now() >= dockDeadline) {
        throw new Error(`workspace.dockCount did not reach 1 within 30 s of clicking ${TILE} ` +
                        `(currentVisibleApp=${JSON.stringify(await visibleApp())})`);
      }
      await sleep(250);
    }
    await app.waitFor(async () => {
      assertEq(await visibleApp(), "test_qml_only", "backend.currentVisibleApp");
    }, { timeout: 10000, interval: 500, description: "the reinstalled app to become front-most" });
    const PAYLOAD_TEXT = "Test QML Only (test_qml_only) v0.1.1";
    await app.waitFor(async () => { await app.expectTexts([PAYLOAD_TEXT]); },
      { timeout: 20000, interval: 500, description: "the reinstalled app's payload text to render" });
    // Launching moves the tile between Repeaters (same name, new object): still one.
    assertEq(await tileCount(), 1, `${TILE} instances with the app open`);
  }));

// B5. A lower version over a higher one, with the app open. PMUI finds the
// package installed at 0.2.1, sees the file carries 0.1.1, and raises
// confirm_upgrade with mode 1 (PackageTypes/UpgradeMode: Downgrade), never
// confirm_install — so the shell draws the upgradeCascade dialog in its
// downgrade wording (ConfirmationDialog.qml: "Downgrade Package?", the body
// leading "Downgrade '<label>' to v0.1.1.", the button "Downgrade"), and the
// install gate stays closed. The "downgrade" chip in the warning colour sits
// only on dependency-change rows, which this plan has none of, so it is not
// looked for. Confirming is the go-ahead: the running app is unloaded for the
// swap, the installed copy removed, the file extracted, and the package is
// then 0.1.1 on disk — and, reopened, renders its 0.1.1 payload, not the
// 0.2.1 one it rendered before.
results.push(await runTest("install: downgrade gate leads with warning wording",
  async ({ app, userDir, log, note }) => {
    await openPackageManager(app);
    const store = await app.requireObject("pmui.BackendStore");
    const gate = await app.requireObject(INSTALL_GATE);
    const cascade = await app.requireObject(UPGRADE_CASCADE);
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible before any install`);
    assertEq(await app.evalOn(cascade, "visible"), false, `${UPGRADE_CASCADE} visible before any install`);
    const TILE = "sidebar.app.test_qml_only";
    const manifestPath = join(userDir, "plugins", "test_qml_only", "manifest.json");
    const TEXT_0_2_1 = "Test QML Only (test_qml_only) v0.2.1";
    const TEXT_0_1_1 = "Test QML Only (test_qml_only) v0.1.1";
    const countLines = (needle) => log().split("\n").filter((l) => l.includes(needle)).length;
    // The manifest is removed and extracted again during the swap: absent or
    // half-written reads as "not there yet", never as a failure.
    const manifestVersion = () => {
      if (!existsSync(manifestPath)) return null;
      try { return JSON.parse(readFileSync(manifestPath, "utf-8")).version ?? null; }
      catch { return null; }
    };
    const workspace = await app.requireObject("workspace");
    // The evaluate anchor with `backend` in scope that survives dock churn
    // (ui-tests.mjs, waitForVisibleApp).
    const greeting = await app.requireObject("welcomePage.greeting");
    const visibleApp = () => app.evalOn(greeting, "backend.currentVisibleApp");
    const dockCount = () => app.evalOn(workspace, "dockCount");
    // Clicks the tile and waits for the app's dock to be front-most and its
    // payload text to render. A load failure is reported as one. The
    // metadata rescans that follow an install rebuild the sidebar's tiles
    // (launcherApps is a plain list: same name, new object), so a click
    // whose tile was rebuilt between the find and the invoke fails without
    // firing and is simply made again.
    const openApp = async (payloadText, what) => {
      await app.requireObject(TILE, 60000);
      await app.waitFor(async () => { await app.click(TILE); },
        { timeout: 10000, interval: 250, description: `${TILE} to take the click for ${what}` });
      const dockDeadline = Date.now() + 30000;
      for (;;) {
        const loadFailed = await app.findByObjectName(LOAD_FAILED);
        if (loadFailed && (await app.evalOn(loadFailed, "visible")) === true) {
          throw new Error(`${what} failed to load: ${await app.evalOn(loadFailed, "errorMessage")}`);
        }
        if ((await dockCount()) === 1) break;
        if (Date.now() >= dockDeadline) {
          throw new Error(`workspace.dockCount did not reach 1 within 30 s of clicking ${TILE} for ${what} ` +
                          `(currentVisibleApp=${JSON.stringify(await visibleApp())})`);
        }
        await sleep(250);
      }
      await app.waitFor(async () => {
        assertEq(await visibleApp(), "test_qml_only", "backend.currentVisibleApp");
      }, { timeout: 10000, interval: 500, description: `${what} to become front-most` });
      await app.waitFor(async () => { await app.expectTexts([payloadText]); },
        { timeout: 20000, interval: 500, description: `${what} to render ${JSON.stringify(payloadText)}` });
    };
    // Whether `text` renders anywhere: the inverse of expectTexts, the same
    // two probes (a whole `text` property, a substring of the tree dump).
    const textRendered = async (text) => {
      if ((await app.findByProperty("text", text)).length > 0) return true;
      const treeStr = JSON.stringify(await app.getTree({ depth: 60 }));
      return treeStr.includes(JSON.stringify(text).slice(1, -1));
    };

    // The higher version first: the plain gate, confirmed; the app installed,
    // opened and rendering its 0.2.1 payload.
    await installViaPmu(app, FIXTURES.A_0_2_1, { confirm: true });
    await waitGateHidden(app, 5000);
    await app.requireObject(TILE, 60000);
    await app.waitFor(async () => {
      assertEq(manifestVersion(), "0.2.1", "installed manifest.json version");
    }, { timeout: 10000, interval: 250, description: "the installed manifest.json at 0.2.1" });
    await app.waitFor(async () => {
      if (countLines(UI_PLUGIN_INSTALLED_LOG) < 1) throw new Error(`no "${UI_PLUGIN_INSTALLED_LOG}" yet`);
    }, { timeout: 10000, interval: 250, description: "the first install's event line" }).catch(() => {});
    const installedBefore = countLines(UI_PLUGIN_INSTALLED_LOG);
    assertEq(await dockCount(), 0, "workspace.dockCount before opening the app");
    await openApp(TEXT_0_2_1, "the 0.2.1 app");
    note(`0.2.1 open and rendering ${JSON.stringify(TEXT_0_2_1)}`);

    // The lower version. Whichever dialog opens first decides: the install
    // gate showing up for a package already installed is the failure this
    // test exists to catch, so it is sampled on every turn.
    const requested = await installLocalPackage(app, store, FIXTURES.A_0_1_1);
    const openDeadline = Date.now() + 30000;
    for (;;) {
      if ((await app.evalOn(gate, "visible")) === true) {
        const { title, body } = await gateTexts(app, gate);
        throw new Error(`${INSTALL_GATE} opened for the downgrade (title=${JSON.stringify(title)} ` +
                        `body=${JSON.stringify(body)}); want ${UPGRADE_CASCADE} in downgrade mode`);
      }
      if ((await app.evalOn(cascade, "visible")) === true) break;
      if (Date.now() >= openDeadline) {
        throw new Error(`${UPGRADE_CASCADE} did not open within 30 s of installLocalPackage(0.1.1)`);
      }
      await sleep(200);
    }
    note(`upgradeCascade opened ${Date.now() - requested} ms after installLocalPackage(0.1.1)`);

    // The wording, read while the dialog is open (a closed instance keeps
    // stale text). The label is the display name once the package is
    // installed, the raw name otherwise; either is accepted and the one seen
    // is noted. The clause after the version says who signed the file (this
    // fixture: nobody). The sentence about running modules follows only when
    // a loaded DEPENDENT is named (loadedItems); A has none, so the head
    // sentence pair may stand alone.
    assertEq(await app.evalOn(cascade, "moduleName"), "test_qml_only", `${UPGRADE_CASCADE} moduleName`);
    assertEq(await app.evalOn(cascade, "upgradeModeKind"), 1, `${UPGRADE_CASCADE} upgradeModeKind (1 = downgrade)`);
    assertEq(await app.evalOn(cascade, "upgradeTargetVersion"), "0.1.1", `${UPGRADE_CASCADE} upgradeTargetVersion`);
    const { title, body, texts } = await gateTexts(app, cascade);
    if (!texts.includes("Downgrade Package?")) {
      throw new Error(`open upgradeCascade renders no "Downgrade Package?" (texts: ${JSON.stringify(texts)})`);
    }
    assertEq(title, "Downgrade Package?", "upgradeCascade title");
    const BODY_RE = /^Downgrade '(Test QML Only|test_qml_only)' to v0\.1\.1([^.]*)\. The current version will be removed first, then the new one downloaded and installed\./;
    const bodyMatch = BODY_RE.exec(body);
    if (!bodyMatch) {
      throw new Error(`upgradeCascade body ${JSON.stringify(body)} does not start ` +
                      `"Downgrade 'Test QML Only' to v0.1.1. The current version will be removed first, …"`);
    }
    note(`dialog body: ${body}`);
    note(`label rendered as ${JSON.stringify(bodyMatch[1])} (${bodyMatch[1] === "Test QML Only" ? "display name" : "raw name"})`);

    // The button says "Downgrade" — with " anyway" appended when the signer
    // check asks the user to go ahead with an unsigned package, the same
    // suffix the install gate's "Install" carries. Never "Upgrade",
    // "Reinstall" or "Install". Enabled once the signer check has run.
    const confirmButton = await app.requireObject(`${UPGRADE_CASCADE}.confirm`);
    await app.waitFor(async () => {
      assertEq(await app.evalOn(confirmButton, "enabled"), true, `${UPGRADE_CASCADE}.confirm enabled`);
    }, { timeout: 30000, interval: 300, description: "the downgrade dialog's confirm to enable" });
    const buttonText = await app.evalOn(confirmButton, "text");
    if (!/^Downgrade( anyway)?$/.test(buttonText)) {
      throw new Error(`${UPGRADE_CASCADE}.confirm reads ${JSON.stringify(buttonText)}, want "Downgrade"`);
    }
    note(`confirm button: ${JSON.stringify(buttonText)}`);
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible while the downgrade dialog is open`);
    await app.send("callMethod", { objectId: confirmButton.id, method: "clicked" });
    await app.waitFor(async () => {
      assertEq(await app.evalOn(cascade, "visible"), false, `${UPGRADE_CASCADE} visible`);
    }, { timeout: 5000, interval: 100, description: "the downgrade dialog to close" });

    // The swap: within 60 s the manifest on disk says 0.1.1, with no failure
    // reported meanwhile.
    const SWAP_MS = 60000;
    const swapDeadline = Date.now() + SWAP_MS;
    for (;;) {
      const failure = await installFailureWitness(app, log);
      if (failure) throw new Error(`the downgrade was reported as failed via ${failure.via}: ${failure.text}`);
      const version = manifestVersion();
      if (version === "0.1.1") break;
      if (Date.now() >= swapDeadline) {
        throw new Error(`${manifestPath} does not say 0.1.1 within ${SWAP_MS / 1000} s of confirming ` +
                        `(version=${JSON.stringify(version)})`);
      }
      await sleep(300);
    }
    note(`manifest.json at 0.1.1 ${Date.now() - requested} ms after the request`);
    // The install half's event line (the metadata rescan follows it); a run
    // with debug logging off never logs it: bounded, not fatal.
    await app.waitFor(async () => {
      if (countLines(UI_PLUGIN_INSTALLED_LOG) <= installedBefore) throw new Error(`no new "${UI_PLUGIN_INSTALLED_LOG}" yet`);
    }, { timeout: 10000, interval: 250, description: "the downgrade's install event line" }).catch(() => {});

    // Reopen. The cascade unloads the running app for the swap, so its dock
    // is normally gone by now; one still there is closed by hand (closeDock
    // takes a string, so it goes through evaluate), as a fresh load is what
    // is being judged.
    if ((await dockCount()) > 0) {
      note(`dock still open after the swap (dockOrder=${JSON.stringify(await app.evalOn(workspace, "dockOrder"))}); closing it`);
      await app.evalOn(workspace, `closeDock(${JSON.stringify("test_qml_only")})`);
      await app.waitFor(async () => {
        assertEq(await dockCount(), 0, "workspace.dockCount");
      }, { timeout: 10000, interval: 250, description: "the 0.2.1 dock to close" });
    } else {
      note("dock unloaded by the swap");
    }
    await openApp(TEXT_0_1_1, "the 0.1.1 app");
    if (await textRendered(TEXT_0_2_1)) {
      throw new Error(`${JSON.stringify(TEXT_0_2_1)} still renders after the downgrade to 0.1.1`);
    }
    assertEq(manifestVersion(), "0.1.1", "installed manifest.json version with the app open");
    // A failure reported late is still a failure.
    const late = await installFailureWitness(app, log);
    if (late) throw new Error(`a failure was reported via ${late.via} after the downgrade: ${late.text}`);
  }));

// B6. Cancelling the upgrade gate, with the old version open. PMUI finds the
// package installed at 0.1.1, sees the file carries 0.2.1, and raises
// confirm_upgrade with mode 0 (PackageTypes/UpgradeMode: Upgrade): the shell
// draws upgradeCascade in its upgrade wording ("Upgrade Package?"). Cancel
// routes to PackageCoordinator::cancelPendingAction (OverlayDialogs.qml),
// which clears the pending slot and answers PMUI errCancelled — and nothing
// else: no unload (the dock stays), no removal (manifest.json keeps its
// version AND its mtime: PMUI removes the installed copy only once the
// answer is a yes), no second dialog of any mode afterwards (a cancellation
// is not a failure to report), and the slot is free again, so asking once
// more opens a fresh upgrade gate.
results.push(await runTest("install: cancelling the upgrade gate keeps the old version",
  async ({ app, userDir, log, note }) => {
    await openPackageManager(app);
    const store = await app.requireObject("pmui.BackendStore");
    const gate = await app.requireObject(INSTALL_GATE);
    const cascade = await app.requireObject(UPGRADE_CASCADE);
    const installError = await app.requireObject(INSTALL_ERROR);
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible before any install`);
    assertEq(await app.evalOn(cascade, "visible"), false, `${UPGRADE_CASCADE} visible before any install`);
    const TILE = "sidebar.app.test_qml_only";
    const manifestPath = join(userDir, "plugins", "test_qml_only", "manifest.json");
    const TEXT_0_1_1 = "Test QML Only (test_qml_only) v0.1.1";
    const countLines = (needle) => log().split("\n").filter((l) => l.includes(needle)).length;
    const manifestVersion = () => {
      if (!existsSync(manifestPath)) return null;
      try { return JSON.parse(readFileSync(manifestPath, "utf-8")).version ?? null; }
      catch { return null; }
    };
    const workspace = await app.requireObject("workspace");
    // The evaluate anchor with `backend` in scope that survives dock churn
    // (ui-tests.mjs, waitForVisibleApp).
    const greeting = await app.requireObject("welcomePage.greeting");
    const visibleApp = () => app.evalOn(greeting, "backend.currentVisibleApp");
    const dockCount = () => app.evalOn(workspace, "dockCount");
    // The three dialogs a cancelled upgrade must not raise, by name, for the
    // watches below.
    const DIALOGS = [
      [INSTALL_GATE, gate], [UPGRADE_CASCADE, cascade], [INSTALL_ERROR, installError],
    ];
    const visibleDialogs = async () => {
      const open = [];
      for (const [name, obj] of DIALOGS) {
        if ((await app.evalOn(obj, "visible")) === true) open.push(name);
      }
      return open;
    };
    // Whether `text` renders anywhere: the inverse of expectTexts, the same
    // two probes (a whole `text` property, a substring of the tree dump).
    const textRendered = async (text) => {
      if ((await app.findByProperty("text", text)).length > 0) return true;
      const treeStr = JSON.stringify(await app.getTree({ depth: 60 }));
      return treeStr.includes(JSON.stringify(text).slice(1, -1));
    };
    // Raw installLocalPackage(0.2.1), then whichever dialog opens first
    // decides: the install gate showing up for a package already installed
    // is a failure, the upgrade gate is what is wanted. Returns the moment
    // of the request.
    const requestUpgradeGate = async (what) => {
      const requested = await installLocalPackage(app, store, FIXTURES.A_0_2_1);
      const openDeadline = Date.now() + 30000;
      for (;;) {
        if ((await app.evalOn(gate, "visible")) === true) {
          const { title, body } = await gateTexts(app, gate);
          throw new Error(`${INSTALL_GATE} opened for ${what} (title=${JSON.stringify(title)} ` +
                          `body=${JSON.stringify(body)}); want ${UPGRADE_CASCADE} in upgrade mode`);
        }
        if ((await app.evalOn(cascade, "visible")) === true) break;
        if (Date.now() >= openDeadline) {
          throw new Error(`${UPGRADE_CASCADE} did not open within 30 s of installLocalPackage(0.2.1) for ${what}`);
        }
        await sleep(200);
      }
      note(`upgradeCascade opened ${Date.now() - requested} ms after installLocalPackage(0.2.1) for ${what}`);
      // Read while open: a closed instance keeps stale text.
      assertEq(await app.evalOn(cascade, "moduleName"), "test_qml_only", `${UPGRADE_CASCADE} moduleName for ${what}`);
      assertEq(await app.evalOn(cascade, "upgradeModeKind"), 0, `${UPGRADE_CASCADE} upgradeModeKind for ${what} (0 = upgrade)`);
      assertEq(await app.evalOn(cascade, "upgradeTargetVersion"), "0.2.1", `${UPGRADE_CASCADE} upgradeTargetVersion for ${what}`);
      const { title, body, texts } = await gateTexts(app, cascade);
      if (!texts.includes("Upgrade Package?")) {
        throw new Error(`open upgradeCascade renders no "Upgrade Package?" for ${what} (texts: ${JSON.stringify(texts)})`);
      }
      assertEq(title, "Upgrade Package?", `upgradeCascade title for ${what}`);
      note(`dialog body for ${what}: ${body}`);
      return requested;
    };
    // Cancel, and the dialog is gone within 5 s.
    const cancelUpgradeGate = async (what) => {
      await app.click(`${UPGRADE_CASCADE}.cancel`);
      await app.waitFor(async () => {
        assertEq(await app.evalOn(cascade, "visible"), false, `${UPGRADE_CASCADE} visible`);
      }, { timeout: 5000, interval: 100, description: `the upgrade dialog to close after cancelling ${what}` });
    };

    // 0.1.1 first: the plain gate, confirmed; the app installed, opened and
    // rendering its 0.1.1 payload.
    await installViaPmu(app, FIXTURES.A_0_1_1, { confirm: true });
    await waitGateHidden(app, 5000);
    await app.requireObject(TILE, 60000);
    await app.waitFor(async () => {
      assertEq(manifestVersion(), "0.1.1", "installed manifest.json version");
    }, { timeout: 10000, interval: 250, description: "the installed manifest.json at 0.1.1" });
    // The install's own event line (the metadata rescan follows it); a run
    // with debug logging off never logs it: bounded, not fatal.
    await app.waitFor(async () => {
      if (countLines(UI_PLUGIN_INSTALLED_LOG) < 1) throw new Error(`no "${UI_PLUGIN_INSTALLED_LOG}" yet`);
    }, { timeout: 10000, interval: 250, description: "the install's event line" }).catch(() => {});
    assertEq(await dockCount(), 0, "workspace.dockCount before opening the app");
    // The metadata rescans that follow an install rebuild the sidebar's
    // tiles (same name, new object), so a click whose tile was rebuilt
    // between the find and the invoke fails without firing and is made again.
    await app.waitFor(async () => { await app.click(TILE); },
      { timeout: 10000, interval: 250, description: `${TILE} to take the click` });
    const dockDeadline = Date.now() + 30000;
    for (;;) {
      const loadFailed = await app.findByObjectName(LOAD_FAILED);
      if (loadFailed && (await app.evalOn(loadFailed, "visible")) === true) {
        throw new Error(`the 0.1.1 app failed to load: ${await app.evalOn(loadFailed, "errorMessage")}`);
      }
      if ((await dockCount()) === 1) break;
      if (Date.now() >= dockDeadline) {
        throw new Error(`workspace.dockCount did not reach 1 within 30 s of clicking ${TILE} ` +
                        `(currentVisibleApp=${JSON.stringify(await visibleApp())})`);
      }
      await sleep(250);
    }
    await app.waitFor(async () => {
      assertEq(await visibleApp(), "test_qml_only", "backend.currentVisibleApp");
    }, { timeout: 10000, interval: 500, description: "the 0.1.1 app to become front-most" });
    await app.waitFor(async () => { await app.expectTexts([TEXT_0_1_1]); },
      { timeout: 20000, interval: 500, description: `the 0.1.1 app to render ${JSON.stringify(TEXT_0_1_1)}` });
    note(`0.1.1 open and rendering ${JSON.stringify(TEXT_0_1_1)}`);
    // The manifest as installed: what a cancel must leave untouched, down to
    // the mtime and the inode (a removal and re-extraction would move both).
    const before = statSync(manifestPath);
    note(`manifest.json before the upgrade request: mtime ${before.mtime.toISOString()}, inode ${before.ino}`);
    const assertManifestUntouched = async (when) => {
      assertEq(manifestVersion(), "0.1.1", `installed manifest.json version ${when}`);
      const now = statSync(manifestPath);
      if (now.mtimeMs !== before.mtimeMs || now.ino !== before.ino) {
        throw new Error(`manifest.json was rewritten ${when}: mtime ${before.mtime.toISOString()} → ` +
                        `${now.mtime.toISOString()}, inode ${before.ino} → ${now.ino}`);
      }
    };

    // The upgrade request, and the cancel.
    await requestUpgradeGate("the first request");
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible while the upgrade dialog is open`);
    await cancelUpgradeGate("the first request");

    // Nothing moved: the dock is still the one 0.1.1 app, still rendering
    // its payload; the manifest is as installed.
    assertEq(await dockCount(), 1, "workspace.dockCount after the cancel");
    assertEq(await visibleApp(), "test_qml_only", "backend.currentVisibleApp after the cancel");
    if (!(await textRendered(TEXT_0_1_1))) {
      throw new Error(`${JSON.stringify(TEXT_0_1_1)} no longer renders after cancelling the upgrade`);
    }
    await assertManifestUntouched("after the cancel");

    // And nothing follows: 10 s in which no dialog of any mode opens — the
    // install gate, the upgrade gate again, or an "Install Failed" notice
    // for a cancellation — the dock stays, and the manifest stays.
    const QUIET_MS = 10000;
    const quietEnd = Date.now() + QUIET_MS;
    while (Date.now() < quietEnd) {
      const open = await visibleDialogs();
      if (open.length > 0) {
        const details = [];
        for (const name of open) {
          const obj = DIALOGS.find(([n]) => n === name)[1];
          const { title, body } = await gateTexts(app, obj);
          details.push(`${name} (moduleName=${JSON.stringify(await app.evalOn(obj, "moduleName"))} ` +
                       `title=${JSON.stringify(title)} body=${JSON.stringify(body)})`);
        }
        throw new Error(`a dialog opened after the upgrade was cancelled: ${details.join(", ")}`);
      }
      assertEq(await dockCount(), 1, "workspace.dockCount while watching after the cancel");
      await assertManifestUntouched("while watching after the cancel");
      await sleep(400);
    }
    const failure = await installFailureWitness(app, log);
    if (failure) throw new Error(`the cancelled upgrade was reported as failed via ${failure.via}: ${failure.text}`);
    if (!(await textRendered(TEXT_0_1_1))) {
      throw new Error(`${JSON.stringify(TEXT_0_1_1)} no longer renders ${QUIET_MS / 1000} s after the cancel`);
    }
    note(`no dialog, no unload, no rewrite within ${QUIET_MS / 1000} s of the cancel`);

    // The slot was released with the answer: asking again opens a fresh
    // upgrade gate. Cancelled too, so the epilogue finds a quiet app.
    await requestUpgradeGate("the second request");
    await cancelUpgradeGate("the second request");
    assertEq(await dockCount(), 1, "workspace.dockCount after the second cancel");
    await assertManifestUntouched("after the second cancel");
    const late = await installFailureWitness(app, log);
    if (late) throw new Error(`a failure was reported via ${late.via} after the second cancel: ${late.text}`);
  }));

// B7. A package whose dependency no repository carries: fixture A declaring
// `no_such_module`. The problem must end up in front of the user, one of
// three ways, and whichever the product takes is asserted in full:
//   (a) the catalog path — AddApplicationDialog's resolution banner ("Some
//       required packages could not be resolved.") with a Required Packages
//       row for no_such_module. The Add Application dialog is the catalog's,
//       so a local file is not expected to land here.
//   (b) the local-file path — the install gate (whose body says the
//       dependencies could not be determined: there is no repository to
//       resolve a local file against) confirmed, the package installed, and
//       the dependency resolver (PackageCoordinator::refreshDependencyInfo,
//       one resolveFlatDependencies round trip per installed package) then
//       flags the tile: sidebar.app.test_qml_only carries hasMissingDeps, and
//       a click on it raises the Missing Dependencies dialog naming
//       no_such_module (UIPluginManager::loadUiModule's gate) instead of a
//       load.
//   (c) the product refuses the package before installing it: Basecamp's
//       "Install Failed" notice names it, OK closes it, and nothing is on
//       disk. The plan's gate does not list this outcome, so a run that takes
//       it reports a finding; it is still asserted as a surfaced refusal,
//       never a silent drop.
// On every branch the app is never loaded: the uiModulesModel row (when
// there is one) reads isLoaded false and the workspace has no dock. The
// resolver's warnings about the missing dependency are expected; a QML
// error is not (G-ERR).
const MISSING_DEPS = "confirmationDialog.missingDeps";
const RESOLUTION_BANNER = "addApplicationDialog.resolutionBanner";
const RESOLUTION_BANNER_TEXT = "Some required packages could not be resolved.";
const MISSING_DEP_NAME = "no_such_module";
// backend.uiModulesModel's roles (ModuleInstanceRoles in
// app/interfaces/BasecampModelRoles.h; ui-tests.mjs reads them the same way).
const UI_MODULE_ROLE = { name: "Qt.UserRole + 1", isLoaded: "Qt.UserRole + 9" };

results.push(await runTest("install: unresolvable dependency is surfaced",
  async ({ app, userDir, log, note }) => {
    await openPackageManager(app);
    const store = await app.requireObject("pmui.BackendStore");
    const gate = await app.requireObject(INSTALL_GATE);
    const installError = await app.requireObject(INSTALL_ERROR);
    const missingDeps = await app.requireObject(MISSING_DEPS);
    assertEq(await app.evalOn(gate, "visible"), false, `${INSTALL_GATE} visible before any install`);
    assertEq(await app.evalOn(installError, "visible"), false, `${INSTALL_ERROR} visible before any install`);
    assertEq(await app.evalOn(missingDeps, "visible"), false, `${MISSING_DEPS} visible before any install`);
    const TILE = "sidebar.app.test_qml_only";
    const pluginDir = join(userDir, "plugins", "test_qml_only");
    const workspace = await app.requireObject("workspace");
    // The evaluate anchor with `backend` in scope (ui-tests.mjs, moduleModelRow).
    const greeting = await app.requireObject("welcomePage.greeting");
    const dockCount = () => app.evalOn(workspace, "dockCount");
    const uiModuleRow = async () => JSON.parse(await app.evalOn(greeting, `JSON.stringify((() => {
      const m = backend.uiModulesModel;
      for (let i = 0; i < m.rowCount(); i += 1) {
        const idx = m.index(i, 0);
        if (String(m.data(idx, ${UI_MODULE_ROLE.name})) === "test_qml_only")
          return { isLoaded: m.data(idx, ${UI_MODULE_ROLE.isLoaded}) };
      }
      return null;
    })())`));
    const visible = async (obj) => (await app.evalOn(obj, "visible")) === true;
    // The banner belongs to the Add Application dialog, which is in the tree
    // only while that dialog is open: absent reads as not shown.
    const bannerShown = async () => {
      const banner = await app.findByObjectName(RESOLUTION_BANNER);
      return banner !== null && (await visible(banner));
    };
    // The tile, with its marker read in the same breath: the metadata
    // rescans rebuild the launcher's tiles (same name, new object), so a
    // tile found and then read may be gone in between — that reads as "not
    // yet", never as a failure.
    const tileState = async () => {
      const tile = await app.findByObjectName(TILE);
      if (!tile) return null;
      try {
        return {
          tile,
          hasMissingDeps: await app.evalOn(tile, "hasMissingDeps"),
          depBlockKind: await app.evalOn(tile, "depBlockKind"),
          loaded: await app.evalOn(tile, "loaded"),
        };
      } catch {
        return null;
      }
    };

    // The request, and whichever dialog surfaces first within 30 s.
    const requested = await installLocalPackage(app, store, FIXTURES.A_0_1_1_NO_SUCH_DEP);
    let first = null;
    const openDeadline = Date.now() + 30000;
    for (;;) {
      if (await visible(gate)) { first = "gate"; break; }
      if (await visible(installError)) { first = "error"; break; }
      if (Date.now() >= openDeadline) {
        throw new Error(`neither ${INSTALL_GATE} nor ${INSTALL_ERROR} opened within 30 s of ` +
                        `installLocalPackage (a package with an unresolvable dependency went unanswered)`);
      }
      await sleep(200);
    }
    note(`${first === "gate" ? INSTALL_GATE : INSTALL_ERROR} opened ${Date.now() - requested} ms after the request`);

    if (first === "gate") {
      // Read while open: a closed instance keeps stale text. The body leads
      // with the package and the version; whether it admits the dependencies
      // could not be determined is noted, as that is the sentence that tells
      // the user something unseen may follow.
      assertEq(await app.evalOn(gate, "moduleName"), "test_qml_only", `${INSTALL_GATE} moduleName`);
      const { title, body } = await gateTexts(app, gate);
      assertEq(title, "Install Package?", "install gate title");
      if (!/^Install '(Test QML Only|test_qml_only)' v0\.1\.1\b/.test(body)) {
        throw new Error(`install gate body ${JSON.stringify(body)} does not start "Install 'test_qml_only' v0.1.1"`);
      }
      note(`gate body: ${body}`);
      note(body.includes("could not be determined")
        ? "the gate says the dependencies could not be determined"
        : "the gate does NOT say the dependencies could not be determined");
      const confirmButton = await app.requireObject(`${INSTALL_GATE}.confirm`);
      await app.waitFor(async () => {
        assertEq(await app.evalOn(confirmButton, "enabled"), true, `${INSTALL_GATE}.confirm enabled`);
      }, { timeout: 30000, interval: 300, description: "the install gate's confirm to enable" });
      await app.send("callMethod", { objectId: confirmButton.id, method: "clicked" });
      await waitGateHidden(app, 5000);
    }

    // Dependency resolution: up to 30 s for the first of the three witnesses.
    // The flagged tile is what the local-file path produces; the banner is
    // the catalog's; an "Install Failed" notice is the refusal.
    let outcome = null;
    const resolveDeadline = Date.now() + 30000;
    for (;;) {
      const tile = await tileState();
      if (tile && tile.hasMissingDeps === true) { outcome = "tile"; break; }
      if (await bannerShown()) { outcome = "banner"; break; }
      if (await visible(installError)) { outcome = "error"; break; }
      if (Date.now() >= resolveDeadline) {
        throw new Error(`30 s after the ${first === "gate" ? "gate was confirmed" : "request"}: no ` +
                        `${TILE} with hasMissingDeps, no ${RESOLUTION_BANNER}, no ${INSTALL_ERROR} ` +
                        `(tile: ${tile ? JSON.stringify({ hasMissingDeps: tile.hasMissingDeps, loaded: tile.loaded }) : "absent"}, ` +
                        `plugins/test_qml_only ${existsSync(pluginDir) ? "exists" : "absent"}): ` +
                        "the unresolvable dependency was dropped silently");
      }
      await sleep(300);
    }
    note(`surfaced as ${outcome} ${Date.now() - requested} ms after the request`);

    if (outcome === "banner") {
      // (a) The catalog's banner — not expected for a local file, but a
      // product that routes there must then name the package it could not
      // resolve.
      const banner = await app.findByObjectName(RESOLUTION_BANNER);
      const text = await app.evalOn(banner, "text");
      if (typeof text !== "string" || !text.startsWith(RESOLUTION_BANNER_TEXT)) {
        throw new Error(`${RESOLUTION_BANNER} reads ${JSON.stringify(text)}, want it to start ` +
                        `${JSON.stringify(RESOLUTION_BANNER_TEXT)}`);
      }
      note(`banner: ${text}`);
      const row = await app.findByObjectName(`packageRow.name.${MISSING_DEP_NAME}`);
      if (!row) throw new Error(`no Required Packages row (packageRow.name.${MISSING_DEP_NAME}) under the banner`);
      assertEq(await visible(installError), false, `${INSTALL_ERROR} visible alongside the banner`);
      const tile = await tileState();
      if (tile && tile.hasMissingDeps === true) {
        throw new Error(`${TILE} is flagged hasMissingDeps as well as the banner: two outcomes, want exactly one`);
      }
      note("outcome (a): the catalog resolution banner — not the local-file path the plan expects");
    } else if (outcome === "tile") {
      // (b) The flagged tile. A click on it opens the Missing Dependencies
      // dialog naming no_such_module and loads nothing. The tile may be
      // rebuilt by a rescan between the find and the click: made again.
      const flagged = await app.waitFor(async () => {
        const t = await tileState();
        if (!t) throw new Error(`${TILE} not in the QML tree`);
        return t;
      }, { timeout: 5000, interval: 250, description: `${TILE} to settle after the rescan` });
      note(`tile flagged: hasMissingDeps=${flagged.hasMissingDeps} depBlockKind=${JSON.stringify(flagged.depBlockKind)} loaded=${flagged.loaded}`);
      assertEq(flagged.loaded, false, `${TILE} loaded while flagged`);
      if (!existsSync(join(pluginDir, "manifest.json"))) {
        throw new Error(`${pluginDir}/manifest.json missing although ${TILE} is flagged`);
      }
      await app.waitFor(async () => { await app.click(TILE); },
        { timeout: 10000, interval: 250, description: `${TILE} to take the click` });
      await app.waitFor(async () => {
        assertEq(await visible(missingDeps), true, `${MISSING_DEPS} visible`);
      }, { timeout: 10000, interval: 200, description: "the Missing Dependencies dialog to open" });
      // Read while open. The items are blocker maps ({ name, kind, detail });
      // the rows render as "• <name>" and the body says what the set is.
      assertEq(await app.evalOn(missingDeps, "moduleName"), "test_qml_only", `${MISSING_DEPS} moduleName`);
      const items = JSON.parse(await app.evalOn(missingDeps, "JSON.stringify(items)"));
      const names = items.map((it) => (typeof it === "string" ? it : (it && it.name) || ""));
      if (!names.includes(MISSING_DEP_NAME)) {
        throw new Error(`${MISSING_DEPS} items ${JSON.stringify(items)} do not name ${MISSING_DEP_NAME}`);
      }
      const summary = await app.evalOn(missingDeps, "blockSummary");
      const { title, body, texts } = await gateTexts(app, missingDeps);
      note(`dialog: title=${JSON.stringify(title)} blockSummary=${JSON.stringify(summary)} body=${JSON.stringify(body)}`);
      assertEq(title, "Missing Dependencies", `${MISSING_DEPS} title`);
      if (!/^'(Test QML Only|test_qml_only)' cannot be loaded because the following modules are not installed:$/.test(body)) {
        throw new Error(`${MISSING_DEPS} body ${JSON.stringify(body)} does not read ` +
                        `"'Test QML Only' cannot be loaded because the following modules are not installed:"`);
      }
      // The row as rendered: in the dialog's own subtree, or — the list's
      // delegates may sit under a view item the walk does not descend into —
      // anywhere in the scene (this dialog is open for the first time, so no
      // stale copy of the row can stand in).
      if (!texts.some((t) => t.startsWith(`• ${MISSING_DEP_NAME}`))) {
        await app.expectTexts([`• ${MISSING_DEP_NAME}`]).catch((e) => {
          throw new Error(`${MISSING_DEPS} renders no "• ${MISSING_DEP_NAME}" row (${e.message}; ` +
                          `dialog texts: ${JSON.stringify(texts)})`);
        });
      }
      // OK closes it, and the click loaded nothing.
      await app.click(`${MISSING_DEPS}.confirm`);
      await app.waitFor(async () => {
        assertEq(await visible(missingDeps), false, `${MISSING_DEPS} visible`);
      }, { timeout: 5000, interval: 100, description: "the Missing Dependencies dialog to close" });
      const after = await app.waitFor(async () => {
        const t = await tileState();
        if (!t) throw new Error(`${TILE} not in the QML tree`);
        return t;
      }, { timeout: 5000, interval: 250, description: `${TILE} after the dialog` });
      assertEq(after.hasMissingDeps, true, `${TILE} hasMissingDeps after the dialog`);
      assertEq(after.loaded, false, `${TILE} loaded after the dialog`);
      // Exactly one outcome: no refusal notice, no catalog banner.
      assertEq(await visible(installError), false, `${INSTALL_ERROR} visible alongside the flagged tile`);
      assertEq(await bannerShown(), false, `${RESOLUTION_BANNER} visible alongside the flagged tile`);
      const failure = await installFailureWitness(app, log);
      if (failure) throw new Error(`the install was reported as failed via ${failure.via} although the package is installed and flagged: ${failure.text}`);
      note("outcome (b): installed, tile flagged, Missing Dependencies dialog names no_such_module");
    } else {
      // (c) Refused before anything installed. The plan does not list this
      // outcome: asserted as a refusal the user can see, and reported.
      const name = await app.evalOn(installError, "moduleName");
      const message = await app.evalOn(installError, "errorMessage");
      const { title, body } = await gateTexts(app, installError);
      note(`Install Failed notice: moduleName=${JSON.stringify(name)} title=${JSON.stringify(title)} ` +
           `body=${JSON.stringify(body)} errorMessage=${JSON.stringify(message)}`);
      assertEq(title, "Install Failed", `${INSTALL_ERROR} title`);
      const named = [name, body, message].some((s) => typeof s === "string" && /test_qml_only|Test QML Only/.test(s));
      if (!named) {
        throw new Error(`${INSTALL_ERROR} does not name the package: moduleName=${JSON.stringify(name)} ` +
                        `body=${JSON.stringify(body)} errorMessage=${JSON.stringify(message)}`);
      }
      if (typeof message !== "string" || message.length === 0) {
        throw new Error(`${INSTALL_ERROR} carries no error message: the refusal gives no reason`);
      }
      await dismissInstallError(app);
      // Nothing installed: no tile within 5 s, no directory, no row.
      await expectAbsent(app, TILE, 5000);
      if (existsSync(pluginDir)) throw new Error(`${pluginDir} exists after the package was refused`);
      assertEq(await bannerShown(), false, `${RESOLUTION_BANNER} visible alongside the refusal`);
      note("outcome (c): refused before installing — an outcome the plan's gate does not list (finding)");
    }

    // On every branch: the app is not loaded and nothing is docked.
    const row = await uiModuleRow();
    if (outcome === "error") {
      if (row !== null) throw new Error(`backend.uiModulesModel has a test_qml_only row after the refusal: ${JSON.stringify(row)}`);
    } else {
      if (row === null) throw new Error("test_qml_only not in backend.uiModulesModel");
      assertEq(row.isLoaded, false, "uiModulesModel isLoaded for test_qml_only");
    }
    assertEq(await dockCount(), 0, "workspace.dockCount");
    const loadFailed = await app.findByObjectName(LOAD_FAILED);
    if (loadFailed && (await visible(loadFailed))) {
      throw new Error(`a load was attempted and failed: ${await app.evalOn(loadFailed, "errorMessage")}`);
    }
  }));

const ran = results.filter((r) => r !== null);
const passed = ran.filter((r) => r === "pass").length;
const failed = ran.filter((r) => r === "fail").length;
const skipped = ran.filter((r) => r === "skip").length;
console.log(`\n${passed} passed, ${failed} failed, ${skipped} skipped`);
console.log(`Total elapsed: ${((Date.now() - suiteStart) / 1000).toFixed(1)}s`);
process.exit(failed > 0 ? 1 : 0);
