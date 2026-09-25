#!/usr/bin/env node
// ---------------------------------------------------------------------------
// logos-basecamp runtime-process tests
//
// The app spawns its runtime (logos_runtime) and reaches it only through module
// calls. What that buys is measured here, in what each process has mapped:
//   - capability_module, the token store, is mapped in logos_runtime and not in
//     the app;
//   - package_manager and package_downloader each run in a logos_host_plain of
//     their own, never in the runtime or the app;
//   - SIGTERM on the app takes the runtime and every host down with it.
//
// The probe that finds capability_module in logos_runtime is the control: a
// probe that sees nothing would fail there, before the absences mean anything.
//
// Usage: node tests/runtime-process-tests.mjs <app-binary>
// ---------------------------------------------------------------------------

import { spawn, execFileSync } from "node:child_process";
import { readFileSync, readlinkSync } from "node:fs";
import { basename } from "node:path";
import { waitForExit } from "./fixtures/harness.mjs";

const APP_BIN = process.argv[2];
if (!APP_BIN) {
  console.error("Usage: node tests/runtime-process-tests.mjs <app-binary>");
  process.exit(1);
}

// From PATH: macOS's own /bin/ps is setuid, which a nix build may not exec.
const PS = "ps";
const LSOF = "lsof";
const READY_MS = 90000;
const EXIT_MS = 20000;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// pid -> { ppid, name } for every process. Linux's comm stops at 15
// characters ("logos_host_plai"), so there the name is the executable's.
function processes() {
  const out = execFileSync(PS, ["-A", "-o", "pid=,ppid=,comm="], { encoding: "utf-8" });
  const table = new Map();
  for (const line of out.split("\n")) {
    const m = line.trim().match(/^(\d+)\s+(\d+)\s+(.+)$/);
    if (!m) continue;
    let name = basename(m[3].trim());
    if (process.platform === "linux") {
      try { name = basename(readlinkSync(`/proc/${m[1]}/exe`)); } catch {}
    }
    table.set(Number(m[1]), { ppid: Number(m[2]), name });
  }
  return table;
}

function children(table, pid, name) {
  return [...table].filter(([, p]) => p.ppid === pid && p.name === name).map(([child]) => child);
}

// A bundle's launcher may run the app as its child rather than exec it.
function descendant(table, pid, name, depth = 3) {
  for (const [child, p] of table) {
    if (p.ppid !== pid) continue;
    if (p.name === name) return child;
    const deeper = depth > 1 ? descendant(table, child, name, depth - 1) : 0;
    if (deeper) return deeper;
  }
  return 0;
}

// The files a process has mapped, by name.
function mapped(pid) {
  let text = "";
  try {
    text = process.platform === "linux"
      ? readFileSync(`/proc/${pid}/maps`, "utf-8")
      : execFileSync(LSOF, ["-p", String(pid), "-Fn"], { encoding: "utf-8" });
  } catch {
    return new Set();
  }
  const names = new Set();
  for (const line of text.split("\n")) {
    const path = process.platform === "linux" ? line.split(/\s+/).slice(5).join(" ") : line.slice(1);
    if (path.startsWith("/")) names.add(basename(path));
  }
  return names;
}

const plugin = (names, module) => [...names].some((n) => n.startsWith(`${module}_plugin.`));
// A zombie counts as gone: nothing runs in it.
function alive(pid) {
  try {
    const state = execFileSync(PS, ["-o", "stat=", "-p", String(pid)], { encoding: "utf-8" }).trim();
    return state !== "" && !state.startsWith("Z");
  } catch {
    return false;
  }
}

const app = spawn(APP_BIN, ["-platform", "offscreen"], {
  stdio: ["ignore", "pipe", "pipe"],
  env: { ...process.env, QT_QPA_PLATFORM: "offscreen" },
});
const log = [];
app.stdout.on("data", (d) => log.push(d));
app.stderr.on("data", (d) => log.push(d));

const failures = [];
const check = (ok, what) => {
  console.log(`  ${ok ? "OK  " : "FAIL"} ${what}`);
  if (!ok) failures.push(what);
};

let runtime = 0;
let hosts = [];
let owner = app.pid; // the process that spawned the runtime
try {
  // Ready: the runtime holds capability_module, and both package modules are up.
  let seen = null;
  for (const deadline = Date.now() + READY_MS; Date.now() < deadline; await sleep(500)) {
    if (app.exitCode !== null) throw new Error(`the app exited early (code ${app.exitCode})`);
    const table = processes();
    runtime = descendant(table, app.pid, "logos_runtime");
    if (!runtime) continue;
    owner = table.get(runtime).ppid;
    hosts = children(table, runtime, "logos_host_plain");
    const maps = new Map([[owner, mapped(owner)], [runtime, mapped(runtime)],
                          ...hosts.map((h) => [h, mapped(h)])]);
    const holder = (module) => hosts.find((h) => plugin(maps.get(h), module)) || 0;
    if (plugin(maps.get(runtime), "capability_module") && holder("package_manager")
        && holder("package_downloader")) {
      seen = { maps, manager: holder("package_manager"), downloader: holder("package_downloader") };
      break;
    }
  }
  if (!runtime) {
    const inApp = plugin(mapped(app.pid), "capability_module");
    throw new Error("the app spawned no logos_runtime"
                    + (inApp ? " (capability_module is mapped in the app itself)" : ""));
  }
  if (!seen) throw new Error("capability_module or a package module never came up where expected");

  console.log(`\nlogos-basecamp runtime-process tests (${process.platform})\n`);
  const appMaps = seen.maps.get(owner);
  const runtimeMaps = seen.maps.get(runtime);
  check(plugin(runtimeMaps, "capability_module"), "capability_module is mapped in logos_runtime");
  for (const module of ["capability_module", "modules_state", "package_manager", "package_downloader"])
    check(!plugin(appMaps, module), `${module} is not mapped in the app`);
  for (const module of ["package_manager", "package_downloader"])
    check(!plugin(runtimeMaps, module), `${module} is not mapped in logos_runtime`);
  check(seen.manager !== seen.downloader, "package_manager and package_downloader have a host each");
  for (const host of [seen.manager, seen.downloader])
    check(!plugin(seen.maps.get(host), "capability_module"), `capability_module is not mapped in host ${host}`);

  app.kill("SIGTERM");
  const exit = await waitForExit(app, EXIT_MS);
  check(exit !== null && exit.code === 0, `SIGTERM ends the app cleanly (${JSON.stringify(exit)})`);
  let gone = false;
  for (const deadline = Date.now() + 10000; !gone && Date.now() < deadline; await sleep(250))
    gone = ![runtime, ...hosts].some(alive);
  check(gone, "logos_runtime and its hosts went down with the app");
} catch (e) {
  failures.push(e.message);
  console.log(`  FAIL ${e.message}`);
} finally {
  if (app.exitCode === null && app.signalCode === null) {
    app.kill("SIGKILL");
    await waitForExit(app, 2000);
  }
}

if (failures.length > 0) {
  process.stderr.write("--- app output ---\n" + Buffer.concat(log).toString("utf-8") + "\n--- end ---\n");
  console.log(`\n${failures.length} failed`);
  process.exit(1);
}
console.log("\nall passed");
