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

function waitForExit(child, ms) {
  return new Promise((resolve) => {
    if (child.exitCode !== null || child.signalCode !== null) {
      return resolve({ code: child.exitCode, signal: child.signalCode });
    }
    const timer = setTimeout(() => resolve(null), ms);
    child.once("exit", (code, signal) => {
      clearTimeout(timer);
      resolve({ code, signal });
    });
  });
}

// Body-return protocol: throw = FAIL; return {skip: reason} = SKIP; else PASS.
class SkipTest {
  constructor(reason) { this.reason = reason; }
}
function skipTest(reason) { return new SkipTest(reason); }

async function runTest(name, body) {
  process.stdout.write(`  ${name} ... `);
  const child = spawnApp();
  const logChunks = [];
  child.stdout.on("data", (d) => logChunks.push(d));
  child.stderr.on("data", (d) => logChunks.push(d));

  let outcome = "pass";
  let err = null;
  let skipReason = null;
  try {
    await waitForInspector();
    // Give plugins time to finish loading so shutdown exercises the full teardown path.
    await new Promise((r) => setTimeout(r, APP_WARMUP_MS));
    const ret = await body(child);
    if (ret instanceof SkipTest) {
      outcome = "skip";
      skipReason = ret.reason;
    }
  } catch (e) {
    outcome = "fail";
    err = e;
  } finally {
    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGKILL");
      await waitForExit(child, 2000);
    }
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

async function assertGracefulExit(child) {
  const exit = await waitForExit(child, SHUTDOWN_WAIT_MS);
  if (!exit) throw new Error(`did not exit within ${SHUTDOWN_WAIT_MS}ms`);
  if (exit.signal) throw new Error(`terminated by signal ${exit.signal} instead of orderly exit`);
  if (exit.code !== 0) throw new Error(`exit code ${exit.code}, want 0`);
}

async function findByObjectName(inspector, name) {
  // findByProperty on objectName reliably finds the tagged object
  // regardless of how its other properties (QKeySequence, etc.) get
  // JSON-serialised by the MCP server.
  const res = await inspector.send("findByProperty", {
    property: "objectName", value: name,
  });
  return (res.matches ?? [])[0] || null;
}

// The hide-to-tray branch of Window::closeEvent only runs when a system
// tray is actually available. In offscreen CI (Xvfb without a tray daemon,
// or QT_QPA_PLATFORM=offscreen) there is no tray, so closing the window
// quits instead of hiding — that's correct behaviour, just not the branch
// we want to exercise. The tray-Quit action isn't even constructed when
// the tray is unavailable, so its object won't exist to find.
async function trayIsAvailable(inspector) {
  const trayAction = await findByObjectName(inspector, "logosTrayQuitAction");
  return trayAction != null;
}


// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
console.log(`\nlogos-basecamp shutdown tests (${process.platform})\n`);
const results = [];

results.push(await runTest("SIGTERM triggers graceful shutdown", async (child) => {
  child.kill("SIGTERM");
  await assertGracefulExit(child);
}));

results.push(await runTest("SIGINT triggers graceful shutdown", async (child) => {
  child.kill("SIGINT");
  await assertGracefulExit(child);
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
    await assertGracefulExit(child);
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
    await assertGracefulExit(child);
  }));
}

// Hide-to-tray: Window.close() must NOT quit when a tray is available.
// Covers ⌘W / red-button / Alt+F4 / window-X — they all end up in
// Window::closeEvent, which either hides (tray present) or quits (no tray).
results.push(await runTest("Window.close() hides to tray, does not quit", async (child) => {
  const inspector = new Inspector();
  await inspector.connect();
  if (!(await trayIsAvailable(inspector))) {
    inspector.disconnect();
    return skipTest("no system tray in this environment (offscreen/Xvfb without tray daemon)");
  }
  const win = await findByObjectName(inspector, "logosMainWindow");
  if (!win) {
    inspector.disconnect();
    throw new Error("Window objectName=logosMainWindow not found");
  }
  const res = await inspector.send("callMethod", { objectId: win.id, method: "close" });
  inspector.disconnect();
  if (res.error) throw new Error(`callMethod(close) failed: ${res.error}`);
  // Give the event loop time to (not) process a shutdown.
  await new Promise((r) => setTimeout(r, 3000));
  if (child.exitCode !== null || child.signalCode !== null) {
    throw new Error(
      `app exited (code=${child.exitCode}, signal=${child.signalCode}) — closeEvent should have hidden to tray`
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
  await assertGracefulExit(child);
}));

const passed = results.filter((r) => r === "pass").length;
const skipped = results.filter((r) => r === "skip").length;
const failed = results.filter((r) => r === "fail").length;
console.log(`\n${passed} passed, ${skipped} skipped, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
