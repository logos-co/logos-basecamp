#!/usr/bin/env node
// ---------------------------------------------------------------------------
// logos-basecamp shutdown tests
//
// Verifies every supported quit gesture actually terminates the process
// cleanly (exit code 0, via the orderly teardown in app/main.cpp:224-254).
//
// Each test spawns a fresh app instance because triggering shutdown kills
// the app — the shared-instance model in ui-tests.mjs doesn't fit.
//
// Coverage:
//   - SIGTERM  (kill, systemd, desktop-session logout)
//   - SIGINT   (Ctrl+C from a terminal launch)
//   - Linux only: Ctrl+Q QShortcut wired to Window::quitApplication
//   - macOS only: Quit QAction (QuitRole) wired to Window::quitApplication
//
// Usage:
//   node tests/shutdown-tests.mjs <app-binary>
//
// Set LOGOS_QT_MCP to override the framework path (nix builds set this
// automatically). Default: ./result-mcp
// ---------------------------------------------------------------------------

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { spawn } from "node:child_process";
import net from "node:net";
import {
  SHUTDOWN_TIER,
  assertCleanTeardown as assertCleanTeardownImpl,
  findByObjectName,
  scanForQmlErrors,
  waitForExit,
} from "./fixtures/harness.mjs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(__dirname, "..");
const qtMcpRoot = process.env.LOGOS_QT_MCP || resolve(projectRoot, "result-mcp");
const { Inspector } = await import(resolve(qtMcpRoot, "test-framework/framework.mjs"));

const APP_BIN = process.argv[2];
if (!APP_BIN) {
  console.error("Usage: node tests/shutdown-tests.mjs <app-binary>");
  process.exit(1);
}

const HOST = process.env.QML_INSPECTOR_HOST || "localhost";
const PORT = parseInt(process.env.QML_INSPECTOR_PORT || "3768", 10);
const INSPECTOR_WAIT_MS = 15000;
const SHUTDOWN_WAIT_MS = 10000;
const APP_WARMUP_MS = 2000;

function spawnApp() {
  return spawn(APP_BIN, ["-platform", "offscreen"], {
    stdio: ["ignore", "pipe", "pipe"],
    env: {
      ...process.env,
      QT_QPA_PLATFORM: "offscreen",
      QT_FORCE_STDERR_LOGGING: "1",
    },
  });
}

async function waitForInspector() {
  const deadline = Date.now() + INSPECTOR_WAIT_MS;
  while (Date.now() < deadline) {
    try {
      await new Promise((resolve, reject) => {
        const sock = net.createConnection({ host: HOST, port: PORT });
        sock.once("connect", () => { sock.destroy(); resolve(); });
        sock.once("error", reject);
      });
      return;
    } catch {
      await new Promise((r) => setTimeout(r, 300));
    }
  }
  throw new Error(`inspector never came up on ${HOST}:${PORT}`);
}

// Body-return protocol: throw = FAIL; return {skip: reason} = SKIP; else PASS.
class SkipTest {
  constructor(reason) { this.reason = reason; }
}
function skipTest(reason) { return new SkipTest(reason); }

let currentRun = null;

// opts: { xfail } — loud on unexpected pass; { tier: "full" } — nightly only.
async function runTest(name, body, opts = {}) {
  process.stdout.write(`  ${name} ... `);
  if (opts.tier === "full" && SHUTDOWN_TIER !== "full") {
    console.log(`\x1b[33mSKIP\x1b[0m — tier "full" test under SHUTDOWN_TIER=${SHUTDOWN_TIER}`);
    return "skip";
  }
  const child = spawnApp();
  const logChunks = [];
  child.stdout.on("data", (d) => logChunks.push(d));
  child.stderr.on("data", (d) => logChunks.push(d));
  const closed = new Promise((r) => child.once("close", r));
  currentRun = {
    log: () => Buffer.concat(logChunks).toString("utf-8"),
    closed,
  };

  let outcome = "pass";
  let err = null;
  let skipReason = null;
  try {
    await waitForInspector();
    // Give plugins time to finish loading so shutdown exercises the full teardown path.
    await new Promise((r) => setTimeout(r, APP_WARMUP_MS));
    // G-ERR scans only startup output — teardown legitimately emits noise.
    const startupChunkCount = logChunks.length;
    const ret = await body(child);
    if (ret instanceof SkipTest) {
      outcome = "skip";
      skipReason = ret.reason;
    } else {
      const qmlErrors = scanForQmlErrors(
        Buffer.concat(logChunks.slice(0, startupChunkCount)).toString("utf-8"));
      if (qmlErrors.length > 0) {
        throw new Error(
          `G-ERR: QML error(s) during startup:\n  ${qmlErrors.join("\n  ")}`);
      }
    }
  } catch (e) {
    outcome = "fail";
    err = e;
  } finally {
    currentRun = null;
    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGKILL");
      await waitForExit(child, 2000);
    }
  }

  if (outcome === "pass" && opts.xfail) {
    outcome = "fail";
    err = new Error(
      `XPASS: test passed but is marked xfail (${opts.xfail}) — the fix ` +
      `landed, remove the xfail marker`);
  } else if (outcome === "fail" && opts.xfail) {
    console.log(`\x1b[33mXFAIL\x1b[0m (${opts.xfail}) — ${err.message}`);
    return "pass";
  }

  if (outcome === "pass") {
    console.log("\x1b[32mOK\x1b[0m");
  } else if (outcome === "skip") {
    console.log(`\x1b[33mSKIP\x1b[0m — ${skipReason}`);
  } else {
    console.log(`\x1b[31mFAIL\x1b[0m — ${err.message}`);
    process.stderr.write("--- app output ---\n");
    process.stderr.write(Buffer.concat(logChunks).toString("utf-8"));
    process.stderr.write("--- end app output ---\n");
  }
  return outcome;
}

async function assertCleanTeardown(child) {
  await assertCleanTeardownImpl(child, {
    waitMs: SHUTDOWN_WAIT_MS,
    log: currentRun?.log,
    closed: currentRun?.closed,
  });
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
console.log(`\nlogos-basecamp shutdown tests (${process.platform}, tier: ${SHUTDOWN_TIER})\n`);
const suiteStart = Date.now();
const results = [];

results.push(await runTest("SIGTERM triggers graceful shutdown", async (child) => {
  child.kill("SIGTERM");
  await assertCleanTeardown(child);
}));

results.push(await runTest("SIGINT triggers graceful shutdown", async (child) => {
  child.kill("SIGINT");
  await assertCleanTeardown(child);
}));

if (process.platform === "linux") {
  results.push(await runTest("Linux: Ctrl+Q QShortcut is wired and quits", async (child) => {
    const inspector = new Inspector();
    await inspector.connect();
    const shortcut = await findByObjectName(inspector, "logosQuitShortcut");
    if (!shortcut) {
      inspector.disconnect();
      throw new Error("QShortcut objectName=logosQuitShortcut not found on the Window");
    }
    if (!(shortcut.type || "").startsWith("QShortcut")) {
      inspector.disconnect();
      throw new Error(`logosQuitShortcut object is ${shortcut.type}, expected QShortcut`);
    }
    // Emit activated() — this is the exact path a real keypress takes.
    const res = await inspector.send("callMethod", {
      objectId: shortcut.id, method: "activated",
    });
    inspector.disconnect();
    if (res.error) throw new Error(`callMethod(activated) failed: ${res.error}`);
    await assertCleanTeardown(child);
  }));
}

if (process.platform === "darwin") {
  results.push(await runTest("macOS: Quit QAction (⌘Q / QuitRole) is wired and quits", async (child) => {
    const inspector = new Inspector();
    await inspector.connect();
    const action = await findByObjectName(inspector, "logosQuitAction");
    if (!action) {
      inspector.disconnect();
      throw new Error("QAction objectName=logosQuitAction not found (menu bar not created)");
    }
    if (!(action.type || "").startsWith("QAction")) {
      inspector.disconnect();
      throw new Error(`logosQuitAction object is ${action.type}, expected QAction`);
    }
    const res = await inspector.send("callMethod", {
      objectId: action.id, method: "trigger",
    });
    inspector.disconnect();
    if (res.error) throw new Error(`callMethod(trigger) failed: ${res.error}`);
    await assertCleanTeardown(child);
  }));
}

// Since 3a73d33 closing the window never quits on any platform (tray/dock convention).
results.push(await runTest("Window.close() does not quit; app keeps running (tray/dock convention)", async (child) => {
  const inspector = new Inspector();
  await inspector.connect();
  const win = await findByObjectName(inspector, "logosMainWindow");
  if (!win) {
    inspector.disconnect();
    throw new Error("Window objectName=logosMainWindow not found");
  }
  const res = await inspector.send("callMethod", { objectId: win.id, method: "close" });
  inspector.disconnect();
  if (res.error) throw new Error(`callMethod(close) failed: ${res.error}`);
  await new Promise((r) => setTimeout(r, 3000));
  if (child.exitCode !== null || child.signalCode !== null) {
    throw new Error(
      `app exited (code=${child.exitCode}, signal=${child.signalCode}) — closing the window must not quit`
    );
  }
}));

// Tray "Quit" action: must terminate the process. Same wiring as ⌘Q on mac
// / Ctrl+Q on Linux, but a distinct connection worth guarding.
results.push(await runTest("Tray Quit QAction is wired and quits", async (child) => {
  const inspector = new Inspector();
  await inspector.connect();
  const action = await findByObjectName(inspector, "logosTrayQuitAction");
  if (!action) {
    inspector.disconnect();
    return skipTest("no system tray in this environment — tray Quit action is only constructed when the tray is available");
  }
  if (!(action.type || "").startsWith("QAction")) {
    inspector.disconnect();
    throw new Error(`logosTrayQuitAction object is ${action.type}, expected QAction`);
  }
  const res = await inspector.send("callMethod", { objectId: action.id, method: "trigger" });
  inspector.disconnect();
  if (res.error) throw new Error(`callMethod(trigger) failed: ${res.error}`);
  await assertCleanTeardown(child);
}));

const passed = results.filter((r) => r === "pass").length;
const skipped = results.filter((r) => r === "skip").length;
const failed = results.filter((r) => r === "fail").length;
console.log(`\n${passed} passed, ${skipped} skipped, ${failed} failed`);
console.log(`Total elapsed: ${((Date.now() - suiteStart) / 1000).toFixed(1)}s`);
process.exit(failed > 0 ? 1 : 0);
