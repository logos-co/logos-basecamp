// Shared suite helpers — the framework itself lives in logos-qt-mcp (not editable here).

import { execFileSync } from "node:child_process";
import {
  existsSync, mkdirSync, readdirSync, readFileSync, statSync,
} from "node:fs";
import { join, resolve } from "node:path";
import { tmpdir } from "node:os";

export const SHUTDOWN_TIER =
  (process.env.SHUTDOWN_TIER || "pr").trim().toLowerCase();

export function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

export async function withTimeout(promise, ms, what) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(
      () => reject(new Error(`timed out after ${ms}ms waiting for ${what}`)),
      ms);
  });
  try {
    return await Promise.race([promise, timeout]);
  } finally {
    clearTimeout(timer);
  }
}

export function waitForExit(child, ms) {
  return new Promise((resolveExit) => {
    if (child.exitCode !== null || child.signalCode !== null) {
      return resolveExit({ code: child.exitCode, signal: child.signalCode });
    }
    const timer = setTimeout(() => resolveExit(null), ms);
    child.once("exit", (code, signal) => {
      clearTimeout(timer);
      resolveExit({ code, signal });
    });
  });
}

export async function findByObjectName(inspector, name) {
  const res = await inspector.send("findByProperty", {
    property: "objectName", value: name,
  });
  return (res.matches ?? [])[0] || null;
}

export async function expectAbsent(app, target, ms = 3000, opts = {}) {
  const { intervalMs = 400, treeDepth = 50 } = opts;
  const deadline = Date.now() + ms;
  for (;;) {
    const byName = await app.inspector.send("findByProperty", {
      property: "objectName", value: target,
    });
    if ((byName.matches ?? []).length > 0) {
      throw new Error(
        `expectAbsent: objectName "${target}" is present (expected absent)`);
    }
    const tree = await app.getTree({ depth: treeDepth });
    if (JSON.stringify(tree).includes(target)) {
      throw new Error(
        `expectAbsent: "${target}" appeared in the QML tree (expected absent)`);
    }
    if (Date.now() >= deadline) return;
    await sleep(Math.min(intervalMs, Math.max(1, deadline - Date.now())));
  }
}

// Returns distinct consecutive values so tests can assert transient stages.
export async function pollProperty(app, objectId, prop, ms, opts = {}) {
  const { intervalMs = 100, until = null } = opts;
  const start = Date.now();
  const samples = [];
  for (;;) {
    const res = await app.inspector.send("evaluate", {
      objectId, expression: prop,
    });
    if (res.error) {
      throw new Error(`pollProperty: evaluate("${prop}") failed: ${res.error}`);
    }
    const value = res.result;
    if (samples.length === 0 || samples[samples.length - 1].value !== value) {
      samples.push({ t: Date.now() - start, value });
    }
    if (until && until(value)) return samples;
    if (Date.now() - start >= ms) return samples;
    await sleep(intervalMs);
  }
}

export async function assertResponsive(app, opts = {}) {
  const { timeoutMs = 5000 } = opts;
  try {
    await withTimeout(app.getTree({ depth: 1 }), timeoutMs, "inspector round-trip");
  } catch (e) {
    throw new Error(`G-ALIVE: app is not responding to the inspector — ${e.message}`);
  }
}

// G-ERR: deliberately narrow — a broad log-regex tripped on unrelated stderr
// noise (nix/smoke-test.nix). File mode (BASECAMP_APP_LOG) no-ops when unset.
const QML_ERROR_RE =
  /\.qml:\d+(?::\d+)?:?\s.*(Error|error:|is not a type|is not defined|No such file|Cannot assign|Unable to assign)/;

export function scanForQmlErrors(text) {
  return text.split("\n").filter((line) => QML_ERROR_RE.test(line));
}

const qmlErrorLogPath = process.env.BASECAMP_APP_LOG || null;
let qmlErrorBaselineOffset = 0;

export function markQmlErrorBaseline() {
  if (!qmlErrorLogPath) return;
  try {
    qmlErrorBaselineOffset = statSync(qmlErrorLogPath).size;
  } catch {
    qmlErrorBaselineOffset = 0;
  }
}

export function assertNoNewQmlErrors(text = null) {
  let candidate = "";
  if (text !== null) {
    candidate = text;
  } else if (qmlErrorLogPath) {
    try {
      candidate = readFileSync(qmlErrorLogPath, "utf-8").slice(qmlErrorBaselineOffset);
    } catch {
      return; // log vanished — nothing to assert against
    }
  } else {
    return; // no log source wired — G-ERR is a no-op in this configuration
  }
  const errors = scanForQmlErrors(candidate);
  if (errors.length > 0) {
    throw new Error(
      `G-ERR: ${errors.length} new QML error(s):\n  ${errors.join("\n  ")}`);
  }
}

// Adds the G-ERR/G-ALIVE epilogue and { xfail } support (loud on unexpected pass).
export function makeTest(frameworkTest) {
  return function test(name, body, opts = {}) {
    const { xfail, ...frameworkOpts } = opts;
    frameworkTest(name, async (app) => {
      markQmlErrorBaseline();
      let bodyError = null;
      try {
        await body(app);
      } catch (e) {
        bodyError = e;
      }
      let epilogueError = null;
      try {
        await assertResponsive(app);
        assertNoNewQmlErrors();
      } catch (e) {
        epilogueError = e;
      }
      if (bodyError) {
        if (xfail) {
          console.log(`    XFAIL (${xfail}): ${bodyError.message}`);
          if (epilogueError) throw epilogueError;
          return;
        }
        throw bodyError; // the body's failure outranks epilogue noise
      }
      if (xfail) {
        throw new Error(
          `XPASS: test passed but is marked xfail (${xfail}) — the fix ` +
          `landed, remove the xfail marker`);
      }
      if (epilogueError) throw epilogueError;
    }, frameworkOpts);
  };
}

// G-EXIT: exit 0, no signal, complete final log line, no orphans or partial files.
export async function assertCleanTeardown(child, opts = {}) {
  const {
    waitMs = 10000, log = null, closed = null,
    userDir = null, partialBaseline = null,
  } = opts;

  const exit = await waitForExit(child, waitMs);
  if (!exit) throw new Error(`G-EXIT: did not exit within ${waitMs}ms`);
  if (exit.signal) {
    throw new Error(
      `G-EXIT: terminated by signal ${exit.signal} instead of orderly exit`);
  }
  if (exit.code !== 0) throw new Error(`G-EXIT: exit code ${exit.code}, want 0`);

  if (log) {
    // 'exit' can fire with data still in flight — wait for the streams to close.
    if (closed) await withTimeout(closed, 3000, "stdio streams to close");
    const text = log();
    if (text.length > 0 && !text.endsWith("\n")) {
      throw new Error(
        `G-EXIT: log truncated mid-line at exit — last bytes: ` +
        JSON.stringify(text.slice(-120)));
    }
  }

  if (userDir) {
    assertNoOrphanProcesses(userDir, [child.pid, process.pid]);
    if (partialBaseline) assertNoNewPartialFiles(userDir, partialBaseline);
  }
}

export function assertNoOrphanProcesses(userDir, excludePids = []) {
  let psOut;
  try {
    psOut = execFileSync("ps", ["-eo", "pid=,args="], { encoding: "utf-8" });
  } catch {
    return; // ps unavailable (bare container) — nothing to assert against
  }
  const needle = resolve(userDir);
  const orphans = [];
  for (const line of psOut.split("\n")) {
    if (!line.includes(needle)) continue;
    const pid = parseInt(line.trim().split(/\s+/)[0], 10);
    if (Number.isNaN(pid) || excludePids.includes(pid)) continue;
    orphans.push(line.trim());
  }
  if (orphans.length > 0) {
    throw new Error(
      `G-EXIT: orphan process(es) still on the user-dir:\n  ${orphans.join("\n  ")}`);
  }
}

const PARTIAL_FILE_RE = /\.(tmp|temp|part|partial|download)$|(^|\/)\.#/;

export function snapshotPartialFiles(userDir) {
  const found = new Set();
  if (!existsSync(userDir)) return found;
  const walk = (dir) => {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, entry.name);
      if (entry.isDirectory()) walk(p);
      else if (PARTIAL_FILE_RE.test(p)) found.add(p);
    }
  };
  walk(userDir);
  return found;
}

export function assertNoNewPartialFiles(userDir, baseline) {
  const now = snapshotPartialFiles(userDir);
  const fresh = [...now].filter((p) => !baseline.has(p));
  if (fresh.length > 0) {
    throw new Error(
      `G-EXIT: new temp/partial file(s) left behind:\n  ${fresh.join("\n  ")}`);
  }
}

let userDirCounter = 0;

export function makeUserDir(prefix = "basecamp-test-user") {
  const dir = join(
    tmpdir(), `${prefix}-${process.pid}-${Date.now()}-${userDirCounter++}`);
  mkdirSync(join(dir, "modules"), { recursive: true });
  mkdirSync(join(dir, "plugins"), { recursive: true });
  return dir;
}

// Install a local .lgx via PMU's installLocalPackage + Basecamp's install gate.
// Requires the Package Manager view opened once (pmui.BackendStore is lazy).
export async function installViaPmu(app, lgxPath, opts = {}) {
  const { confirm = true, timeoutMs = 30000, intervalMs = 300 } = opts;
  const inspector = app.inspector;

  const store = await findByObjectName(inspector, "pmui.BackendStore");
  if (!store) {
    throw new Error(
      "installViaPmu: pmui.BackendStore not found — open the Package " +
      "Manager view first (it is created lazily on first activation)");
  }
  const fileUrl = lgxPath.startsWith("file://")
    ? lgxPath
    : `file://${resolve(lgxPath)}`;
  const call = await inspector.send("callMethod", {
    objectId: store.id, method: "installLocalPackage", args: [fileUrl],
  });
  if (call.error) {
    throw new Error(`installViaPmu: installLocalPackage failed: ${call.error}`);
  }

  // Gate open = visible; closed dialog instances keep stale texts in the tree.
  const gate = await findByObjectName(inspector, "confirmationDialog.installGate");
  if (!gate) throw new Error("installViaPmu: installGate dialog object not found");
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const res = await inspector.send("evaluate", {
      objectId: gate.id, expression: "visible",
    });
    if (res.result === true) break;
    if (Date.now() >= deadline) {
      throw new Error(`installViaPmu: install gate did not open within ${timeoutMs}ms`);
    }
    await sleep(intervalMs);
  }

  const buttonName = confirm
    ? "confirmationDialog.installGate.confirm"
    : "confirmationDialog.installGate.cancel";
  const button = await findByObjectName(inspector, buttonName);
  if (!button) throw new Error(`installViaPmu: ${buttonName} not found`);
  const clicked = await inspector.send("callMethod", {
    objectId: button.id, method: "clicked",
  });
  if (clicked.error) {
    throw new Error(`installViaPmu: clicking ${buttonName} failed: ${clicked.error}`);
  }
}
