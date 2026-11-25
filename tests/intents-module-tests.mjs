// intents_module end to end: Basecamp claims intents_module, an app's backend
// asks right on a click in its view, Basecamp opens it and the chooser comes
// up, and the core module that provides answers the requester alone. A request
// with no recent click behind it waits instead of popping up. Run by `nix build .#intents-module-test` through
// nix/mkPluginTest.nix.
import { readdirSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";

const root = process.env.LOGOS_QT_MCP || new URL("../result-mcp", import.meta.url).pathname;
const { test, run } = await import(resolve(root, "test-framework/framework.mjs"));

const userDir = process.env.BASECAMP_TEST_USER_DIR;

function sessionLog() {
  const logs = join(userDir, "logs");
  const file = readdirSync(logs).filter((f) => /^basecamp_.*\.log$/.test(f)).sort().pop();
  if (!file) throw new Error(`no session log in ${logs}`);
  return readFileSync(join(logs, file), "utf8");
}

async function find(app, objectName) {
  return ((await app.findByProperty("objectName", objectName)).matches ?? [])[0] ?? null;
}

async function requireObject(app, objectName, timeout = 30000) {
  let match = null;
  await app.waitFor(async () => {
    match = await find(app, objectName);
    if (!match) throw new Error(`no ${objectName}`);
  }, { timeout, interval: 250, description: `${objectName} to exist` });
  return match;
}

async function evalOn(app, objectId, expression) {
  const res = await app.inspector.send("evaluate", { objectId, expression });
  if (res.error) throw new Error(`evaluate("${expression}") failed: ${res.error}`);
  return res.result;
}

async function textOf(app, objectName) {
  const label = await requireObject(app, objectName);
  return evalOn(app, label.id, "text");
}

async function waitForText(app, objectName, expected, timeout = 15000) {
  let last = "";
  await app.waitFor(async () => {
    last = await textOf(app, objectName);
    if (last !== expected) throw new Error(`${objectName}: ${last}`);
  }, { timeout, interval: 250, description: `${objectName} to read "${expected}"` });
}

async function click(app, objectName) {
  const target = await requireObject(app, objectName);
  await app.inspector.send("click", { objectId: target.id });
}

async function chooserOpen(app) {
  const dialog = await find(app, "intentChooserDialog");
  return dialog !== null && (await evalOn(app, dialog.id, "visible").catch(() => false)) === true;
}

test("intents_module: Basecamp is its presenter", async (app) => {
  await app.waitFor(async () => { await app.expectTexts(["Backend: connected"]); },
    { timeout: 60000, interval: 500, description: "the requester's backend" });
  if (!sessionLog().includes("[intents_module] presenter claimed"))
    throw new Error("intents_module was not claimed");
});

// The shell's sidebar anchor lives in Basecamp's own engine, where `backend` is.
async function shellEval(app, expression) {
  const anchor = await requireObject(app, "sidebar.app.intent_module_requester");
  return evalOn(app, anchor.id, expression);
}

async function providerLoaded(app) {
  return shellEval(app, `(() => {
    const m = backend.coreModulesModel;
    for (let i = 0; i < m.rowCount(); i += 1)
      if (m.data(m.index(i, 0), Qt.UserRole + 1) === "intent_echo_provider")
        return m.data(m.index(i, 0), Qt.UserRole + 9) === true ? "yes" : "no";
    return "absent";
  })()`);
}

// Started by Basecamp when the person picks it, below.
test("intents_module: the core-module provider is not running before it is chosen", async (app) => {
  let loaded = "";
  await app.waitFor(async () => {
    loaded = await providerLoaded(app);
    if (loaded === "absent") throw new Error("intent_echo_provider is not listed yet");
  }, { timeout: 30000, interval: 500, description: "intent_echo_provider to be listed" });
  if (loaded !== "no") throw new Error(`intent_echo_provider: ${loaded}`);
});

test("intents_module: a click reaches the chooser, and the provider answers the requester", async (app) => {
  await click(app, "requester.ask");

  await app.waitFor(async () => {
    if (!(await chooserOpen(app))) throw new Error("no chooser");
  }, { timeout: 15000, interval: 250, description: "the chooser for test.module_echo" });
  await click(app, "intentChooserConfirm");

  await waitForText(app, "requester.status", "Status: #1 clicked: echo: hello (for intent_module_requester_backend)");
  await waitForText(app, "requester.waiting", "Waiting for an answer: 0");
  // The module list refreshes on Basecamp's 2 s stats tick.
  await app.waitFor(async () => {
    if (await providerLoaded(app) !== "yes") throw new Error("the chosen provider is not running");
  }, { timeout: 10000, interval: 500, description: "the chosen provider to be listed as running" });
});

test("intents_module: a request with no click waits and never pops up", async (app) => {
  await click(app, "requester.askNoClick");
  await waitForText(app, "requester.status", "Status: #2 no click: waiting");

  // Longer than the error floor and a round trip: nothing may come up.
  const until = Date.now() + 3000;
  while (Date.now() < until) {
    if (await chooserOpen(app)) throw new Error("a request with no click raised the chooser");
    await new Promise((r) => setTimeout(r, 250));
  }
  await waitForText(app, "requester.status", "Status: #2 no click: waiting", 1000);
  await waitForText(app, "requester.waiting", "Waiting for an answer: 1", 1000);
  if (!sessionLog().includes("intents_module: pending request"))
    throw new Error("the unclicked request did not reach the pending list");
});

test("intents_module: the pending list holds 4 per app, and cancelling empties it", async (app) => {
  // Three quick clicks are three requests, 3 s apart: #3, #4 and #5.
  for (let i = 0; i < 3; i += 1) await click(app, "requester.askNoClick");
  await waitForText(app, "requester.status", "Status: #5 no click: waiting", 20000);
  await waitForText(app, "requester.waiting", "Waiting for an answer: 4");

  // A fifth waiting request is refused, on the usual floor.
  await click(app, "requester.askNoClick");
  await waitForText(app, "requester.status", "Status: #6 no click: error: unavailable");
  await waitForText(app, "requester.waiting", "Waiting for an answer: 4");

  // The app withdraws its own requests; each settles as cancelled.
  await click(app, "requester.cancelWaiting");
  await waitForText(app, "requester.waiting", "Waiting for an answer: 0");
  const history = await textOf(app, "requester.history");
  for (const n of [2, 3, 4, 5]) {
    if (!history.includes(`#${n} no click: error: cancelled`))
      throw new Error(`#${n} was not cancelled:\n${history}`);
  }

  // With the list empty again, the next one waits instead of being refused.
  await click(app, "requester.askNoClick");
  await waitForText(app, "requester.status", "Status: #7 no click: waiting");
});

test("intents_module: only the Package Manager may ask to confirm an install", async (app) => {
  await click(app, "requester.confirmInstall");
  await waitForText(app, "requester.status", "Status: #8 clicked: error: unavailable");
  if (!sessionLog().includes("refused basecamp.packages.confirm_install from intent_module_requester_backend"))
    throw new Error("the confirmation was not refused by the restriction");
  if (await shellEval(app, "backend.currentActiveSectionIndex") !== 0)
    throw new Error("the refused confirmation moved the shell");
});

// The module's process, by the plugin it maps. Linux only (/proc).
function intentsModulePid() {
  for (const entry of readdirSync("/proc")) {
    if (!/^\d+$/.test(entry)) continue;
    let maps = "";
    try { maps = readFileSync(`/proc/${entry}/maps`, "utf8"); } catch { continue; }
    if (maps.includes("intents_module_plugin")) return Number(entry);
  }
  return null;
}

test("intents_module: Basecamp loads it again after it dies, and claims it again", async (app) => {
  const pid = intentsModulePid();
  if (!pid) throw new Error("no intents_module process");
  process.kill(pid, "SIGKILL");
  await app.waitFor(async () => {
    if (!sessionLog().includes("intents_module: claimed again after it restarted"))
      throw new Error("not claimed again yet");
  }, { timeout: 30000, interval: 500, description: "intents_module to be claimed again" });

  // Everything it knew came back: the requester's backend, the provider.
  await click(app, "requester.ask");
  await app.waitFor(async () => {
    if (!(await chooserOpen(app))) throw new Error("no chooser");
  }, { timeout: 15000, interval: 250, description: "the chooser after the restart" });
  await click(app, "intentChooserConfirm");
  await waitForText(app, "requester.status", "Status: #9 clicked: echo: hello (for intent_module_requester_backend)");
});

async function waitForVisibleApp(app, name, description, timeout = 30000) {
  await app.waitFor(async () => {
    const visible = await shellEval(app, "backend.currentVisibleApp");
    if (visible !== name) throw new Error(`visible: ${visible}`);
  }, { timeout, interval: 250, description });
}

// The provider is an app that is not open: Basecamp starts it when the
// module asks, raises it, the person answers there, and goes back.
test("intents_module: an app provides: started, shown, answered, and back", async (app) => {
  await click(app, "requester.askApp");
  await app.waitFor(async () => {
    if (!(await chooserOpen(app))) throw new Error("no chooser");
  }, { timeout: 15000, interval: 250, description: "the chooser for test.app_echo" });
  await click(app, "intentChooserConfirm");

  await waitForVisibleApp(app, "intent_app_provider", "the provider app to be started and shown");
  await waitForText(app, "provider.request", "Request: intent_module_requester_backend asks: hi app");
  await click(app, "provider.approve");

  await waitForText(app, "requester.status", "Status: #10 clicked: echo: hi app (for intent_module_requester_backend)");
  await waitForVisibleApp(app, "intent_module_requester", "the person to be taken back", 10000);
  if (!sessionLog().includes("Restricted intent_app_provider_backend to intent_app_provider"))
    throw new Error("the provider's backend was not restricted before it started");
});

// Not through intents_module, a module can't hand a provider a request: the
// core module checks its caller, and an app's backend takes calls only from
// its view and intents_module.
test("intents_module: a provider refuses a delivery that did not come through it", async (app) => {
  const root = await requireObject(app, "requester.root");
  await evalOn(app, root.id, `(backend.deliverDirectly("intent_echo_provider", "test.module_echo"), "sent")`);
  await waitForText(app, "requester.direct", "Direct: intent_echo_provider: refused", 20000);
  // Its glue refuses it, or capability_module keeps the call from it.
  await evalOn(app, root.id, `(backend.deliverDirectly("intent_app_provider_backend", "test.app_echo"), "sent")`);
  let direct = "";
  await app.waitFor(async () => {
    direct = await textOf(app, "requester.direct");
    if (!direct.startsWith("Direct: intent_app_provider_backend: ")) throw new Error(direct);
  }, { timeout: 20000, interval: 250, description: "the app provider's answer" });
  if (direct.endsWith(": accepted")) throw new Error(`the app provider took a forged delivery (${direct})`);
  const shown = await textOf(app, "provider.request");
  if (shown.includes("forged")) throw new Error(`the app provider took a forged request: ${shown}`);
});

// Basecamp's own request with one app to answer it goes straight there, with
// no chooser: the click in Basecamp was the choice, as on the QML path.
test("intents_module: Basecamp's own request goes straight to the one app that answers it", async (app) => {
  let chooserSeen = false;
  await shellEval(app, `(backend.showPackageDetails("hello_pkg"), "sent")`);
  await app.waitFor(async () => {
    if (await chooserOpen(app)) chooserSeen = true;
    const shown = await textOf(app, "provider.request");
    if (shown !== "Request: main_ui shows: hello_pkg") throw new Error(shown);
  }, { timeout: 20000, interval: 200, description: "the provider to show Basecamp's request" });
  if (chooserSeen) throw new Error("a chooser came up for Basecamp's own request");
  await click(app, "sidebar.app.intent_module_requester");
  await waitForVisibleApp(app, "intent_module_requester", "the requester to be back in front");
});

// Nothing the module knows provides it: it answers so, and the App Manager
// answers instead.
test("intents_module: Basecamp's own request nothing provides goes to the App Manager", async (app) => {
  await shellEval(app, `(backend.requestPackageInstall("hello_pkg"), "sent")`);
  await app.waitFor(async () => {
    if (!sessionLog().includes("intents_module did not take packages.install; the App Manager answers for hello_pkg"))
      throw new Error("no fallback yet");
  }, { timeout: 10000, interval: 250, description: "the App Manager to answer" });
});

// After the others: it takes the person away from the requester.
test("intents_module: Basecamp provides its own intents through it", async (app) => {
  await click(app, "requester.openSettings");
  await app.waitFor(async () => {
    const section = await shellEval(app, "backend.currentActiveSectionIndex");
    if (section !== 3) throw new Error(`section ${section}`);
  }, { timeout: 15000, interval: 250, description: "Settings to open" });
  await waitForText(app, "requester.status", "Status: #11 clicked: ok");
});

// Last: it leaves intents_module off. Past its third restart Basecamp stops
// asking it, and the App Manager answers Basecamp's own requests.
test("intents_module: once it stays down, the App Manager answers Basecamp's own requests", async (app) => {
  const again = "intents_module: claimed again after it restarted";
  const count = (line) => sessionLog().split(line).length - 1;
  for (let restarts = count(again); restarts < 3; ++restarts) {
    process.kill(intentsModulePid(), "SIGKILL");
    await app.waitFor(async () => {
      if (count(again) <= restarts) throw new Error("not claimed again yet");
    }, { timeout: 30000, interval: 500, description: `restart ${restarts + 1}` });
  }
  process.kill(intentsModulePid(), "SIGKILL");
  await app.waitFor(async () => {
    if (!sessionLog().includes("intents_module stopped again; it is off for this session"))
      throw new Error("still restarting it");
  }, { timeout: 30000, interval: 500, description: "Basecamp to stop restarting it" });

  await shellEval(app, `(backend.showPackageDetails("bye_pkg"), "sent")`);
  await app.waitFor(async () => {
    if (!sessionLog().includes("intents_module did not take packages.show; the App Manager answers for bye_pkg"))
      throw new Error("no fallback yet");
  }, { timeout: 10000, interval: 250, description: "the App Manager to answer" });
  const shown = await textOf(app, "provider.request");
  if (shown.includes("bye_pkg")) throw new Error(`the provider was asked anyway: ${shown}`);
});

await run();
