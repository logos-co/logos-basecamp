#!/usr/bin/env node
// ---------------------------------------------------------------------------
// logos-basecamp UI integration tests
//
// Usage:
//   node tests/ui-tests.mjs                       # run all (app must be running)
//   node tests/ui-tests.mjs modules               # run tests matching "modules"
//   node tests/ui-tests.mjs --ci <app-binary>     # CI mode: launch app, test, kill
//
// Set LOGOS_QT_MCP to override the framework path (nix builds set this automatically).
// Default: ./result-mcp (built via: nix build .#logos-qt-mcp -o result-mcp)
// ---------------------------------------------------------------------------

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { readFileSync, statSync, writeSync } from "node:fs";
import {
  assertResponsive, findByObjectName, makeTest, sleep,
} from "./fixtures/harness.mjs";
import { FIXTURE_A } from "./fixtures/lgx.mjs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(__dirname, "..");
const qtMcpRoot = process.env.LOGOS_QT_MCP || resolve(projectRoot, "result-mcp");
const { test: frameworkTest, run } =
  await import(resolve(qtMcpRoot, "test-framework/framework.mjs"));

// Adds the G-ERR/G-ALIVE epilogue and { xfail } support to every test.
const test = makeTest(frameworkTest);

// run() exits the process itself; writeSync so the line isn't dropped at exit.
const suiteStart = Date.now();
process.on("exit", () => {
  const elapsed = `Total elapsed: ${((Date.now() - suiteStart) / 1000).toFixed(1)}s\n`;
  try {
    writeSync(1, elapsed);
  } catch {
    try {
      writeSync(2, elapsed);
    } catch {
      // Best-effort shutdown logging only; never fail the suite on exit.
    }
  }
});

// Shared with the dedicated `host-services-test` check. Registered here as well
// because this suite is the one CI already runs by name (`nix build
// .#integration-test` / `.#integration-test-bundle`), and it is the suite that
// reported 16 passed / 0 failed against a build where capability_module never
// received its host-services grant and 34 gated calls were refused.
const { assertHostServicesGrantReached } = await import(
  resolve(__dirname, "host-services-assert.mjs")
);

// Helper: click a plugin's sidebar icon and wait for its UI to load.
// Plugins load asynchronously after clicking, so we wait for expected
// content to appear before proceeding.
//
// `opts` is now forwarded to app.click(). It used to be declared and then
// silently dropped; no caller passed anything, so nothing visibly broke — but
// it meant a caller could not disambiguate its click even if it wanted to,
// which is exactly what the package_manager_ui test needed. See sidebarSection.
async function openPlugin(app, name, expectedTexts, opts = {}) {
  const { timeout = 10000, ...clickOpts } = opts;
  await app.click(name, clickOpts);
  await app.waitFor(
    async () => { await app.expectTexts(expectedTexts); },
    { timeout, interval: 500, description: `"${name}" UI to load` }
  );
}

// --- Shared helpers ---------------------------------------------------------
//
// Clicks are signal-level (callMethod "clicked" / evaluate "clicked()"): the
// QML lives in offscreen QQuickWindows where coordinate hit-testing is
// unreliable, and the onClicked handler chain is the same. callMethod cannot
// marshal QString arguments, so methods taking one (closeDock, openFor) go
// through evaluate. evaluate round-trips primitives only: one property per
// call, JSON for anything structured.

const CI_MODE = process.argv.includes("--ci");

// Pins a click to a sidebar SECTION button. findAndClick substring-matches
// `text` breadth-first and a QWidget click never fails, so a bare
// app.click("Package Manager") lands on MainContainer's shallower
// "Loading Package Manager…" placeholder label and reports success.
const sidebarSection = { exact: true, type: "SidebarCircleButton" };

function assertEq(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(
      `${what}=${JSON.stringify(actual)} (expected ${JSON.stringify(expected)})`);
  }
}

function assertType(value, type, what) {
  if (typeof value !== type) {
    throw new Error(`${what}=${JSON.stringify(value)} (expected ${type})`);
  }
  return value;
}

async function findByType(app, typeName) {
  const res = await app.inspector.send("findByType", { typeName });
  if (res.error) throw new Error(`findByType(${typeName}) failed: ${res.error}`);
  return res.matches ?? [];
}

// Waits for an objectName to appear; returns the match.
async function requireObject(app, objectName, timeout = 10000) {
  let obj = null;
  await app.waitFor(async () => {
    obj = await findByObjectName(app.inspector, objectName);
    if (!obj) throw new Error(`${objectName} not in the QML tree`);
  }, { timeout, interval: 500, description: `${objectName} to exist` });
  return obj;
}

// Which of `texts` no descendant of `rootExpr` renders, evaluated in the
// object's own scope.
async function missingTexts(app, objectId, rootExpr, texts) {
  return JSON.parse(await evalOn(app, objectId, `(() => {
    const hasText = (node, expected) => {
      if (!node) return false;
      if (typeof node.text === "string" && node.text.includes(expected)) return true;
      if (!node.children || typeof node.children.length !== "number") return false;
      for (let i = 0; i < node.children.length; i += 1) {
        if (hasText(node.children[i], expected)) return true;
      }
      return false;
    };
    return JSON.stringify(${JSON.stringify(texts)}.filter((t) => !hasText(${rootExpr}, t)));
  })()`));
}

// QQuickWidget hosts carry no objectName; identify them by source URL.
async function quickWidgetHosts(app) {
  const hosts = [];
  for (const m of await findByType(app, "QQuickWidget")) {
    const props = await app.inspector.send("getProperties", { objectId: m.id });
    const prop = (name) => props.properties?.find((p) => p.name === name)?.value;
    hosts.push({ id: m.id, source: prop("source"), visible: prop("visible") });
  }
  return hosts;
}

async function findWelcomePage(app) {
  return (await findByType(app, "WelcomePage"))[0] || null;
}

// The welcome page is the evaluate anchor with `backend` in scope that
// survives dock teardown and sidebar delegate churn.
async function requireWelcomePage(app) {
  let welcome = null;
  await app.waitFor(async () => {
    welcome = await findWelcomePage(app);
    if (!welcome) throw new Error("no WelcomePage instance in the QML tree");
  }, { timeout: 10000, interval: 500, description: "WelcomePage instance to exist" });
  return welcome;
}

const requireWorkspace = (app) => requireObject(app, "workspace");

// Welcome visibility lives on the hosting QQuickWidget, not the QML item: the
// page is a permanent tab, and QMainWindow hides its widget when another
// dock is raised. Falls back to the item's Window attached property.
async function welcomePageHidden(app, welcomeItemId) {
  for (const host of await quickWidgetHosts(app)) {
    if (typeof host.source === "string" && host.source.includes("WelcomePage.qml")
        && typeof host.visible === "boolean") {
      return !host.visible;
    }
  }
  const winVisible = await evalOn(app, welcomeItemId, "Window.visible");
  if (typeof winVisible === "boolean") return !winVisible;
  throw new Error(
    "cannot determine welcome-page visibility: no QQuickWidget sourced from " +
    "WelcomePage.qml, and Window.visible is not a boolean");
}

// Fixture A (tests/fixtures/lgx.mjs) is pre-seeded by nix/integration-test.nix,
// so in --ci its absence is a failure. Against a local app it is a skip:
// logs and returns null.
async function requireFixtureA(app, label, probe, description) {
  let value = null;
  try {
    await app.waitFor(async () => { value = await probe(); },
      { timeout: 10000, interval: 500, description });
  } catch (e) {
    if (!CI_MODE) {
      console.log(`    SKIP: ${label} — fixture A precondition not met: ${e.message}`);
      return null;
    }
    throw new Error(
      `${label} precondition failed (fixture A is pre-seeded in --ci): ${e.message}`);
  }
  return value;
}

const FIXTURE_A_TILE = `sidebar.app.${FIXTURE_A.name}`;

// Launching moves the tile from the unloaded to the loaded Repeater (same
// objectName, new object), so callers re-find it instead of caching.
async function findFixtureATile(app) {
  const tile = await findByObjectName(app.inspector, FIXTURE_A_TILE);
  if (!tile) throw new Error(`${FIXTURE_A_TILE} not in the tree`);
  return tile;
}

const clickFixtureATile = async (app) =>
  invoke(app, (await findFixtureATile(app)).id, "clicked", `clicking ${FIXTURE_A_TILE}`);

// Payload text rendered by fixture A's Main.qml (qmlViewFor in lgx.mjs).
const FIXTURE_A_TEXT =
  `${FIXTURE_A.displayName} (${FIXTURE_A.name}) v${FIXTURE_A.version}`;

async function waitForDockCount(app, workspaceId, expected, description, timeout = 10000) {
  await app.waitFor(async () => {
    assertEq(await evalOn(app, workspaceId, "dockCount"), expected, "WorkspaceArea.dockCount");
  }, { timeout, interval: 250, description });
}

async function waitForVisibleApp(app, anchorId, expected, description) {
  await app.waitFor(async () => {
    assertEq(await evalOn(app, anchorId, "backend.currentVisibleApp"), expected,
             "backend.currentVisibleApp");
  }, { timeout: 10000, interval: 500, description });
}

// Clicks fixture A's tile and waits for its dock to be open and front-most.
// Returns false when the fixture is absent outside --ci (already logged).
async function openFixtureA(app, label, welcomeId, workspaceId) {
  const tile = await requireFixtureA(app, label, () => findFixtureATile(app),
                                     "fixture A sidebar tile to appear");
  if (tile === null) return false;
  await invoke(app, tile.id, "clicked", `clicking ${FIXTURE_A_TILE}`);
  await waitForDockCount(app, workspaceId, 1, "fixture A dock to open");
  await waitForVisibleApp(app, welcomeId, FIXTURE_A.name, "fixture A to become front-most");
  return true;
}

// Closing the last dock also unloads the module (WorkspaceArea::pluginClosed).
async function closeFixtureADock(app, workspaceId, description) {
  await evalOn(app, workspaceId, `closeDock(${JSON.stringify(FIXTURE_A.name)})`);
  await waitForDockCount(app, workspaceId, 0, description, 5000);
}

// --- Welcome page (A1) — runs first: asserts the pre-interaction state ---

test("welcome: first launch shows the welcome page", async (app) => {
  const welcome = await requireWelcomePage(app);
  assertEq(await evalOn(app, welcome.id, "visible"), true, "WelcomePage visible");

  // launcherApps populates asynchronously; count and greeting are read together.
  await app.waitFor(async () => {
    const count = assertType(await evalOn(app, welcome.id, "backend.launcherApps.length"),
                             "number", "backend.launcherApps.length");
    await app.expectTexts([count === 0 ? "Welcome to Basecamp!" : "Welcome Back,"]);
  }, { timeout: 10000, interval: 500, description: "greeting to match backend.launcherApps" });

  const missing = await missingTexts(app, welcome.id, "this",
                                     ["Welcome to Basecamp!", "Welcome Back,"]);
  if (missing.length !== 1) {
    throw new Error(`greetings missing from the welcome page: ${JSON.stringify(missing)} ` +
                    "(expected exactly one of the two)");
  }
});

// --- Welcome page: search, filters and Recently Closed -------------------
// Placed before the navigation tests below, which click the welcome page away.
// All of these drive the page through the inspector rather than synthesised
// keystrokes, because the QML lives in an offscreen QQuickWindow.

// Set a QML property through the inspector. `evaluate` runs in the object's
// scope, so an assignment is the portable way to poke one.
async function setQmlProperty(app, objectName, expression) {
  const obj = await findByObjectName(app.inspector, objectName);
  if (!obj) throw new Error(`no object named "${objectName}"`);
  const res = await app.inspector.send("evaluate", { objectId: obj.id, expression });
  if (res.error) throw new Error(`evaluate("${expression}") failed: ${res.error}`);
  return obj;
}

async function visibilityOf(app, objectName) {
  const obj = await findByObjectName(app.inspector, objectName);
  if (!obj) return null;
  const res = await app.inspector.send("evaluate", {
    objectId: obj.id, expression: "visible",
  });
  if (res.error) throw new Error(`evaluate(visible) on ${objectName}: ${res.error}`);
  return res.result;
}

test("welcome: Recently Closed is hidden until an app has been closed", async (app) => {
  const page = await findWelcomePage(app);
  if (!page) throw new Error("no WelcomePage instance in the QML tree");

  const countRes = await app.inspector.send("evaluate", {
    objectId: page.id, expression: "backend.recentlyClosedApps.length",
  });
  if (countRes.error) {
    throw new Error(`evaluate(recentlyClosedApps.length) failed: ${countRes.error}`);
  }
  const count = countRes.result;
  if (typeof count !== "number") {
    throw new Error(`recentlyClosedApps.length=${JSON.stringify(count)} (expected number)`);
  }

  // The section is bound to the list being non-empty, both ways round: an
  // empty heading over blank space is as wrong as a populated list not showing.
  const visible = await visibilityOf(app, "welcomePage.recentlyClosed");
  if (count === 0 && visible === true) {
    throw new Error("Recently Closed is visible with an empty list");
  }
  if (count > 0 && visible !== true) {
    throw new Error(`Recently Closed hidden with ${count} entries — the list is `
                  + "restored from disk, so this is the startup-race regression");
  }
});

test("welcome: a query swaps Recently Closed for results", async (app) => {
  await setQmlProperty(app, "welcomePage.search", 'text = "a"');

  await app.waitFor(async () => {
    if (await visibilityOf(app, "welcomePage.searchResults") !== true) {
      throw new Error("results section did not appear");
    }
    if (await visibilityOf(app, "welcomePage.recentlyClosed") === true) {
      throw new Error("Recently Closed still visible while searching");
    }
  }, { timeout: 5000, interval: 200, description: "results to replace Recently Closed" });

  // Clearing restores the resting state.
  await setQmlProperty(app, "welcomePage.search", 'text = ""');
  await app.waitFor(async () => {
    if (await visibilityOf(app, "welcomePage.searchResults") === true) {
      throw new Error("results still visible after clearing the query");
    }
  }, { timeout: 5000, interval: 200, description: "results to clear" });
});

test("welcome: a query with no matches explains itself", async (app) => {
  await setQmlProperty(app, "welcomePage.search", 'text = "zzzzz-no-such-package"');

  await app.waitFor(async () => {
    if (await visibilityOf(app, "welcomePage.noResults") !== true) {
      throw new Error("no-results notice did not appear");
    }
  }, { timeout: 5000, interval: 200, description: "no-results notice" });

  await setQmlProperty(app, "welcomePage.search", 'text = ""');
});

test("welcome: the filter chips scope which result rows show", async (app) => {
  await setQmlProperty(app, "welcomePage.search", 'text = "a"');
  await sleep(300);

  const chip = await findByObjectName(app.inspector, "welcomePage.filterPackages");
  if (!chip) throw new Error("packages filter chip not found");
  const clicked = await app.inspector.send("callMethod", {
    objectId: chip.id, method: "clicked",
  });
  if (clicked.error) throw new Error(`callMethod(clicked) failed: ${clicked.error}`);

  // The apps row must yield to the active chip. The packages row may still be
  // empty on a bare install, so only the exclusion is asserted.
  await app.waitFor(async () => {
    if (await visibilityOf(app, "welcomePage.results.apps") === true) {
      throw new Error("applications row still visible under the packages filter");
    }
  }, { timeout: 5000, interval: 200, description: "apps row to hide" });

  await app.inspector.send("callMethod", { objectId: chip.id, method: "clicked" });
  await setQmlProperty(app, "welcomePage.search", 'text = ""');
});

// ⌘K cannot be driven end-to-end here: the inspector cannot synthesise a key
// event into the offscreen QQuickWindow, and that window is never "active", so
// activeFocus is false for every item no matter what has focus. What IS
// assertable is the half that regressed before — that the page still declares
// the shortcut for ShortcutBridge to mirror onto the host. The bridge logs
// "bound N QML shortcut(s)" for the welcome pane when it picks it up.
test("welcome: the page declares a ⌘K shortcut for the bridge to mirror", async (app) => {
  const res = await app.inspector.send("findByType", { typeName: "QQuickShortcut" });
  const shortcuts = res.matches ?? [];
  if (shortcuts.length === 0) throw new Error("no Shortcut declared on the welcome page");

  let found = false;
  for (const sc of shortcuts) {
    const seq = await app.inspector.send("evaluate", {
      objectId: sc.id, expression: "JSON.stringify({ s: nativeText, on: enabled })",
    });
    if (seq.error) continue;
    const info = JSON.parse(seq.result);
    if (typeof info.s === "string" && /K$/i.test(info.s)) {
      if (info.on !== true) throw new Error(`⌘K shortcut present but enabled=${info.on}`);
      found = true;
      break;
    }
  }
  if (!found) {
    throw new Error("no enabled ⌘K shortcut among "
                  + `${shortcuts.length} declared shortcut(s)`);
  }
});

// --- Welcome page (A2) — runs right after A1: navigating clicks the page away ---

test('welcome: "Discover Applications" navigates to Applications', async (app) => {
  // Typed before navigating, asserted cleared after: the welcome search must
  // not survive the page going away. WorkspaceArea clears it from hideEvent
  // because the root Item's `visible` stays true inside the offscreen host,
  // so only a real app can catch this regression.
  await setQmlProperty(app, "welcomePage.search", 'text = "waku"');

  const button = await requireObject(app, "welcomePage.discoverApplications");
  await invoke(app, button.id, "clicked", 'clicking "Discover Applications"');
  await app.waitFor(
    async () => { await app.expectTexts(["Install and manage applications."]); },
    { timeout: 10000, interval: 500, description: "Applications view to render" }
  );

  // The sidebar "Applications" delegate knows the section index it activates
  // (onClicked passes _d.workspaceSections.length + index); only objects in
  // SidebarPanel's delegate scope resolve that expression, which also tells
  // the button apart from same-text headers.
  const sidebarHits = await app.findByProperty("text", "Applications");
  let appsButtonId = null;
  let applicationsIndex = null;
  for (const m of sidebarHits.matches ?? []) {
    const res = await app.inspector.send("evaluate", {
      objectId: m.id, expression: "_d.workspaceSections.length + index",
    });
    if (!res.error && typeof res.result === "number") {
      appsButtonId = m.id;
      applicationsIndex = res.result;
      break;
    }
  }
  if (appsButtonId === null) {
    throw new Error('sidebar "Applications" button (with section index in scope) not found');
  }
  await app.waitFor(async () => {
    assertEq(await evalOn(app, appsButtonId, "backend.currentActiveSectionIndex"),
             applicationsIndex, "backend.currentActiveSectionIndex");
  }, { timeout: 10000, interval: 500, description: "active section to become Applications" });

  // What observably hides the welcome page is its host, WorkspaceArea.
  const workspace = await requireWorkspace(app);
  assertEq(await evalOn(app, workspace.id, "visible"), false, "workspace visible");

  const field = await requireObject(app, "welcomePage.search");
  const text = await evalOn(app, field.id, "text");
  if (text !== "") {
    throw new Error(
      `welcome search still holds ${JSON.stringify(text)} after navigating away ` +
      "(WorkspaceArea::clearWelcomeSearch did not run)");
  }
});

// --- Workspace (A3) — opening an app replaces the welcome page with a dock ---
// Runs after A2 and leaves fixture A's dock open for A4.

test("workspace: opening an app replaces the welcome page with a dock", async (app) => {
  const welcome = await requireWelcomePage(app);
  const workspace = await requireWorkspace(app);
  if (!(await openFixtureA(app, "A3", welcome.id, workspace.id))) return;

  await app.waitFor(async () => {
    if (!(await welcomePageHidden(app, welcome.id))) {
      throw new Error("welcome page is still visible after the dock opened");
    }
  }, { timeout: 5000, interval: 250, description: "welcome page to hide" });

  await app.waitFor(
    async () => { await app.expectTexts([FIXTURE_A_TEXT]); },
    { timeout: 10000, interval: 500, description: "fixture A payload text to render" }
  );
});

// --- Workspace (A4) — closing the last dock brings the welcome page back ---

test("workspace: closing the last dock brings the welcome page back", async (app) => {
  const welcome = await requireWelcomePage(app);
  const workspace = await requireWorkspace(app);

  // A3 leaves the dock open; open it here if not, so the test stands alone.
  if ((await evalOn(app, workspace.id, "dockCount")) !== 1) {
    if (!(await openFixtureA(app, "A4", welcome.id, workspace.id))) return;
  }
  await waitForVisibleApp(app, welcome.id, FIXTURE_A.name, "fixture A to be front-most");

  await closeFixtureADock(app, workspace.id, "workspace dockCount to reach 0");

  await app.waitFor(async () => {
    if (await welcomePageHidden(app, welcome.id)) {
      throw new Error("welcome page is still hidden after closing the last dock");
    }
  }, { timeout: 5000, interval: 250, description: "welcome page to reappear" });

  // Closing unloads fixture A but does not uninstall it, so the greeting is
  // the installed-apps one.
  await app.waitFor(
    async () => { await app.expectTexts(["Welcome Back,"]); },
    { timeout: 5000, interval: 250, description: '"Welcome Back," greeting to render' }
  );
  await waitForVisibleApp(app, welcome.id, "", "currentVisibleApp to clear");
});

// --- Workspace (A5) — re-clicking an open app does not create a second dock ---

test("workspace: re-clicking an open app does not create a second dock", async (app) => {
  const welcome = await requireWelcomePage(app);
  const workspace = await requireWorkspace(app);
  if (!(await openFixtureA(app, "A5", welcome.id, workspace.id))) return;

  // Second click 500 ms after the dock exists. Re-found inside the retry:
  // the delegate may be mid-move between Repeaters, and a duplicate
  // activation click is harmless.
  await sleep(500);
  await app.waitFor(() => clickFixtureATile(app),
    { timeout: 10000, interval: 500, description: "second click on fixture A tile" });

  // dockCount must stay 1 across a settle window: the load path defers
  // through singleShot timers, so a single read could miss a second dock.
  const settleDeadline = Date.now() + 2000;
  for (;;) {
    assertEq(await evalOn(app, workspace.id, "dockCount"), 1,
             "WorkspaceArea.dockCount after re-click");
    if (Date.now() >= settleDeadline) break;
    await sleep(250);
  }
  assertEq(await evalOn(app, welcome.id, "backend.currentVisibleApp"), FIXTURE_A.name,
           "backend.currentVisibleApp after re-click");

  // Fixture A's root is a plain Rectangle, so "one instance" is checked via
  // its host QQuickWidget's source and its unique payload text.
  const fixtureHosts = (await quickWidgetHosts(app))
    .map((h) => h.source)
    .filter((s) => typeof s === "string"
                   && s.includes(`/${FIXTURE_A.name}/`) && s.endsWith("Main.qml"));
  if (fixtureHosts.length !== 1) {
    throw new Error(
      `${fixtureHosts.length} QQuickWidget(s) sourced from fixture A's Main.qml ` +
      `(expected exactly 1): ${JSON.stringify(fixtureHosts)}`);
  }
  const textHits = await app.findByProperty("text", FIXTURE_A_TEXT);
  if (textHits.error) throw new Error(`findByProperty(text=payload) failed: ${textHits.error}`);
  assertEq((textHits.matches ?? []).length, 1, "instances of fixture A's payload text");

  await closeFixtureADock(app, workspace.id, "cleanup: fixture A dock to close");
});

// --- Sidebar (A6) — footer shows the build type, with the version when present ---
//
// Expectations are derived from backend.buildVersion / backend.isPortableBuild:
// nix builds bake "0.0.0-dev" when VERSION is absent, but a non-nix build can
// have an empty buildVersion, leaving the build-type token alone. Scoped to
// sidebar.buildLabel because DashboardView renders its own "Dev build".

test("sidebar: footer shows the build type, with the version when present", async (app) => {
  const footer = await requireObject(app, "sidebar.buildLabel");
  const buildVersion = assertType(await evalOn(app, footer.id, "backend.buildVersion"),
                                  "string", "backend.buildVersion");
  const isPortable = assertType(await evalOn(app, footer.id, "backend.isPortableBuild"),
                                "boolean", "backend.isPortableBuild");
  const text = assertType(await evalOn(app, footer.id, "text"), "string", "footer text");

  // Exact comparison pins the " · " separator and rejects stray suffixes.
  const token = isPortable ? "Portable" : "Dev";
  assertEq(text, buildVersion.length > 0 ? `${buildVersion} · ${token}` : token,
           `footer text (buildVersion=${JSON.stringify(buildVersion)}, ` +
           `isPortableBuild=${isPortable})`);
});

// --- Sidebar (A7) — the active tile follows currentVisibleApp ---
//
// The tile's highlight binds `checked: modelData.name === backend.currentVisibleApp`
// and a section click only flips the content stack, so the tile stays lit
// while Settings is front-most. Re-clicking it goes through launchUIModule →
// navigateToApps, back to workspace section 0. Ends with the dock open.

test("sidebar: active tile follows currentVisibleApp across section switches", async (app) => {
  const welcome = await requireWelcomePage(app);
  const workspace = await requireWorkspace(app);
  const evalOnWelcome = (expression) => evalOn(app, welcome.id, expression);
  // Only the loaded Repeater's delegate carries the checked binding, so
  // every read re-finds the tile.
  const tileChecked = async () => evalOn(app, (await findFixtureATile(app)).id, "checked");

  if (!(await openFixtureA(app, "A7", welcome.id, workspace.id))) return;
  await app.waitFor(async () => {
    assertEq(await evalOnWelcome("backend.currentActiveSectionIndex"), 0,
             "backend.currentActiveSectionIndex after open");
    assertEq(await tileChecked(), true, "tile checked after open");
  }, { timeout: 5000, interval: 250, description: "workspace section active and tile lit" });

  // Section buttons carry no objectName: locate Settings by text + type.
  let settingsButton = null;
  await app.waitFor(async () => {
    const hits = await app.findByProperty("text", "Settings");
    settingsButton = (hits.matches ?? [])
      .find((m) => (m.type ?? "").includes("SidebarCircleButton")) || null;
    if (!settingsButton) throw new Error('sidebar "Settings" SidebarCircleButton not found');
  }, { timeout: 10000, interval: 500, description: '"Settings" sidebar button to exist' });
  const settingsChecked = () => evalOn(app, settingsButton.id, "checked");

  await invoke(app, settingsButton.id, "clicked", "clicking the Settings button");
  await app.waitFor(async () => {
    if ((await evalOnWelcome("backend.currentActiveSectionIndex")) === 0) {
      throw new Error("still on workspace section 0 after the Settings click");
    }
    assertEq(await settingsChecked(), true, "Settings button checked");
  }, { timeout: 10000, interval: 500, description: "Settings section to become active" });

  // Single-shot reads: a retried wait would mask a transient un-light.
  assertEq(await evalOnWelcome("backend.currentVisibleApp"), FIXTURE_A.name,
           "backend.currentVisibleApp while Settings is active");
  assertEq(await tileChecked(), true, "tile checked while Settings is active");

  await app.waitFor(() => clickFixtureATile(app),
    { timeout: 10000, interval: 500, description: "re-click on fixture A tile" });
  await app.waitFor(async () => {
    assertEq(await evalOnWelcome("backend.currentActiveSectionIndex"), 0,
             "backend.currentActiveSectionIndex after re-click");
  }, { timeout: 10000, interval: 500, description: "workspace section to reactivate" });

  assertEq(await tileChecked(), true, "tile checked after returning to the workspace");
  assertEq(await settingsChecked(), false, "Settings button checked after returning");
  assertEq(await evalOn(app, workspace.id, "dockCount"), 1, "WorkspaceArea.dockCount at end");
  assertEq(await evalOnWelcome("backend.currentVisibleApp"), FIXTURE_A.name,
           "backend.currentVisibleApp at end");
});

// --- App Manager helpers -----------------------------------------------------
//
// AppsFilterProxy's search is a fixed-string case-insensitive contains over
// name/displayName/description. appManager.localAppsProxy chains
// matchLocalOnly onto the searched proxy, so its visibleCount tracks the
// search and its sourceModel is the searched ("outer") proxy — the same
// AppsFilterProxy AppManagerView calls appsProxy. Offline, the outer model
// holds the local installs plus package_downloader's default catalog.

// AppsModelRoles (app/interfaces/BasecampModelRoles.h) as evaluate expressions.
const APPS_ROLE = {
  name: "Qt.UserRole + 1",
  repositoryUrl: "Qt.UserRole + 2",
  displayName: "Qt.UserRole + 3",
  description: "Qt.UserRole + 4",
  category: "Qt.UserRole + 5",
  isInstalled: "Qt.UserRole + 14",
  installStatus: "Qt.UserRole + 16",
  installType: "Qt.UserRole + 17",
  installStage: "Qt.UserRole + 29", // PlanInstallStageRole — what the delegates snapshot
};

// Opens the Applications view and returns the appManager.localAppsProxy id.
async function openApplicationsWithProxy(app) {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Install and manage applications."]); },
    { timeout: 10000, interval: 500, description: "Applications view to render" }
  );
  let proxyId = null;
  await app.waitFor(async () => {
    proxyId = await findLocalAppsProxy(app);
    if (proxyId === null) throw new Error("appManager.localAppsProxy not found in QML tree");
  }, { timeout: 10000, interval: 500, description: "localAppsProxy to exist" });
  return proxyId;
}

async function outerRowCount(app, proxyId) {
  return assertType(await evalOn(app, proxyId, "sourceModel.rowCount()"),
                    "number", "outer proxy rowCount()");
}

// One primitive role of outer-model row `i`; unset roles coerce to "" / false / 0.
async function outerRowField(app, proxyId, i, roleExpr, kind = "string") {
  const data = `sourceModel.data(sourceModel.index(${i}, 0), ${roleExpr})`;
  const expr = kind === "string" ? `String(${data} || "")`
    : kind === "bool" ? `${data} === true`
    : `Number(${data} || 0)`;
  return evalOn(app, proxyId, expr);
}

// A search field driven by assigning `text`. That breaks the
// `text: d.searchText` binding, which is harmless: onTextChanged still feeds
// d.searchText, and every test ends with the field cleared.
async function searchFieldOn(app, objectName) {
  const field = await requireObject(app, objectName);
  const get = () => evalOn(app, field.id, "text");
  const set = (value) => evalOn(app, field.id, `text = ${JSON.stringify(value)}`);
  const expectText = async (expected) => assertEq(await get(), expected, `${objectName} text`);
  // Clears leftover text so a recorded baseline is unfiltered.
  const normalize = async () => {
    if (assertType(await get(), "string", `${objectName} text`) === "") return;
    await set("");
    await app.waitFor(() => expectText(""),
      { timeout: 5000, interval: 100, description: `${objectName} to clear` });
  };
  return { id: field.id, get, set, expectText, normalize };
}

// Clears the App Manager search and waits for the recorded counts to return
// and the empty view to hide.
async function clearAppManagerSearch(app, search, proxyId, { preLocal = null, preOuterRows }) {
  const emptyView = await requireObject(app, "appManager.emptyView");
  await search.set("");
  await app.waitFor(async () => {
    await search.expectText("");
    assertEq(await outerRowCount(app, proxyId), preOuterRows,
             "outer proxy rowCount() after clearing the search");
    if (preLocal !== null) {
      assertEq(await evalOn(app, proxyId, "visibleCount"), preLocal,
               "localAppsProxy.visibleCount after clearing the search");
    }
    assertEq(await evalOn(app, emptyView.id, "visible"), false,
             "appManager.emptyView visible after clearing the search");
  }, { timeout: 5000, interval: 250, description: "cleared search to restore the grid" });
}

// Precondition: fixture A installed and at least one local row in the grid.
// Returns localAppsProxy.visibleCount, or null on skip.
async function requireFixtureALocalRow(app, label, proxyId) {
  return requireFixtureA(app, label, async () => {
    await findFixtureATile(app);
    const count = await evalOn(app, proxyId, "visibleCount");
    if (typeof count !== "number" || count < 1) {
      throw new Error(`localAppsProxy.visibleCount=${count} (expected at least 1 local row)`);
    }
    return count;
  }, "fixture A and at least one local row to be present");
}

// --- App Manager (A8) — search narrows the grid to matching apps ---

test("app manager: search narrows the grid to matching apps", async (app) => {
  const proxyId = await openApplicationsWithProxy(app);
  const search = await searchFieldOn(app, "appManager.searchField");
  await search.normalize();
  const preLocal = await requireFixtureALocalRow(app, "A8", proxyId);
  if (preLocal === null) return;
  const preOuterRows = await outerRowCount(app, proxyId);

  // Wrong case proves case-insensitivity. The spaced display name matches
  // only via DisplayNameRole (name and description use the underscored form).
  const matchQuery = FIXTURE_A.displayName.toUpperCase();
  if (matchQuery === FIXTURE_A.displayName) {
    throw new Error(
      `FIXTURE_A.displayName=${JSON.stringify(FIXTURE_A.displayName)} is already ` +
      "upper-case, so the wrong-case leg cannot prove case-insensitivity");
  }
  await search.set(matchQuery);
  await app.waitFor(async () => {
    await search.expectText(matchQuery);
    assertEq(await evalOn(app, proxyId, "visibleCount"), 1,
             "localAppsProxy.visibleCount with wrong-case display-name search");
    assertEq(await outerRowCount(app, proxyId), 1,
             "outer proxy rowCount() with wrong-case display-name search");
  }, { timeout: 5000, interval: 250,
       description: "wrong-case display-name search to narrow to fixture A" });

  const noMatchQuery = `${matchQuery} ZZZ-NO-SUCH-APP`;
  await search.set(noMatchQuery);
  await app.waitFor(async () => {
    await search.expectText(noMatchQuery);
    assertEq(await evalOn(app, proxyId, "visibleCount"), 0,
             "localAppsProxy.visibleCount with non-matching search");
    assertEq(await outerRowCount(app, proxyId), 0,
             "outer proxy rowCount() with non-matching search");
  }, { timeout: 5000, interval: 250, description: "non-matching search to empty the grid" });

  await clearAppManagerSearch(app, search, proxyId, { preLocal, preOuterRows });
});

// --- App Manager (A9) — search with no match shows the empty view ---

test("app manager: search with no match shows the empty view", async (app) => {
  const proxyId = await openApplicationsWithProxy(app);

  // A catalog refresh landing mid-test would cover the empty view and move
  // the counts the restore gate compares against.
  const overlay = await requireObject(app, "appManager.loadingOverlay");
  await app.waitFor(async () => {
    assertEq(await evalOn(app, overlay.id, "visible"), false, "appManager.loadingOverlay visible");
  }, { timeout: 30000, interval: 500, description: "apps loading overlay to hide" });

  const search = await searchFieldOn(app, "appManager.searchField");
  // A plain (non-Loader) child, findable while hidden.
  const emptyView = await requireObject(app, "appManager.emptyView");
  await search.normalize();
  const preLocal = assertType(await evalOn(app, proxyId, "visibleCount"),
                              "number", "localAppsProxy.visibleCount");
  const preOuterRows = await outerRowCount(app, proxyId);

  // title (EmptyView) or text (LogosText), whichever is a non-empty string.
  const emptyViewMessage = async () => {
    for (const prop of ["title", "text"]) {
      const res = await app.inspector.send("evaluate", { objectId: emptyView.id, expression: prop });
      if (!res.error && typeof res.result === "string" && res.result.length > 0) return res.result;
    }
    return "";
  };

  // Every AppRepoSection: the Repeater's repo delegates plus the
  // always-instantiated synthetic local one, so zero matches means the
  // probe itself broke.
  const visibleRepoSectionCount = async () => {
    const matches = await findByType(app, "AppRepoSection");
    if (matches.length === 0) {
      throw new Error("findByType(AppRepoSection) returned no instances");
    }
    let count = 0;
    for (const m of matches) {
      if ((await evalOn(app, m.id, "visible")) === true) count += 1;
    }
    return count;
  };

  // The synthetic local section's header is the lowercase "local". Uses
  // getProperties: a non-visual match has no `visible` and does not count,
  // while a failed round-trip must surface.
  const visibleLocalHeaderCount = async () => {
    const hits = await app.inspector.send("findByProperty", { property: "text", value: "local" });
    if (hits.error) throw new Error(`findByProperty(text="local") failed: ${hits.error}`);
    let count = 0;
    for (const m of (hits.matches ?? [])) {
      const props = await app.inspector.send("getProperties", { objectId: m.id });
      if (props.error) throw new Error(`getProperties(${m.id}) failed: ${props.error}`);
      if (props.properties?.find((p) => p.name === "visible")?.value === true) count += 1;
    }
    return count;
  };

  // "§" is inert data: the filter is fixed-string, not a pattern.
  const noMatchQuery = "zzz-no-such-app-§";
  await search.set(noMatchQuery);
  await app.waitFor(async () => {
    await search.expectText(noMatchQuery);
    assertEq(await outerRowCount(app, proxyId), 0, "outer proxy rowCount() with no-match search");
    assertEq(await evalOn(app, proxyId, "visibleCount"), 0,
             "localAppsProxy.visibleCount with no-match search");
    assertEq(await evalOn(app, emptyView.id, "visible"), true,
             "appManager.emptyView visible with no-match search");
    if ((await emptyViewMessage()).length === 0) {
      throw new Error("appManager.emptyView carries no message (neither title nor text)");
    }
    assertEq(await visibleRepoSectionCount(), 0, "visible AppRepoSections with no-match search");
    assertEq(await visibleLocalHeaderCount(), 0, 'visible "local" headers with no-match search');
  }, { timeout: 5000, interval: 250, description: "no-match search to show the empty view" });

  await clearAppManagerSearch(app, search, proxyId, { preLocal, preOuterRows });
});

// --- App Manager (A10) — search tolerates regex/special/unicode input ---
//
// Regex metacharacters are inert data to the fixed-string filter, so none of
// these inputs may produce QRegularExpression warnings or a stale grid. The
// expected count per input is computed from a snapshot of the searched
// fields, mirroring the C++ filter: "(" matches fixture A's own description,
// and the outer model also holds the default catalog.

test("app manager: search tolerates regex/special/unicode input", async (app) => {
  const proxyId = await openApplicationsWithProxy(app);
  const search = await searchFieldOn(app, "appManager.searchField");
  const emptyView = await requireObject(app, "appManager.emptyView");

  // Scanned from a baseline taken now; no-op when BASECAMP_APP_LOG is unset.
  const appLogPath = process.env.BASECAMP_APP_LOG || null;
  let appLogBaseline = 0;
  if (appLogPath) {
    try { appLogBaseline = statSync(appLogPath).size; } catch { appLogBaseline = 0; }
  }
  const assertNoRegexWarnings = (label) => {
    if (!appLogPath) return;
    let tail = "";
    try {
      tail = readFileSync(appLogPath).subarray(appLogBaseline).toString("utf-8");
    } catch {
      return;
    }
    const hits = tail.split("\n").filter((l) => l.includes("QRegularExpression"));
    if (hits.length > 0) {
      throw new Error(
        `${hits.length} QRegularExpression warning(s) in the app log after input ` +
        `${label}:\n  ${hits.join("\n  ")}`);
    }
  };

  // Keeps the 512-char input out of descriptions and error messages.
  const labelFor = (input) =>
    input.length > 16
      ? `${JSON.stringify(`${input.slice(0, 8)}…`)} (${input.length} chars)`
      : JSON.stringify(input);

  await search.normalize();
  if ((await requireFixtureALocalRow(app, "A10", proxyId)) === null) return;
  const preOuterRows = await outerRowCount(app, proxyId);

  const snapshot = [];
  for (let i = 0; i < preOuterRows; i += 1) {
    snapshot.push({
      name: await outerRowField(app, proxyId, i, APPS_ROLE.name),
      displayName: await outerRowField(app, proxyId, i, APPS_ROLE.displayName),
      description: await outerRowField(app, proxyId, i, APPS_ROLE.description),
    });
  }
  const expectedMatches = (input) => {
    const needle = input.toLowerCase();
    return snapshot.filter((r) =>
      [r.name, r.displayName, r.description].some((v) => v.toLowerCase().includes(needle))
    ).length;
  };

  const inputs = ["(", "[", "*", "\\", ".*", "日本語", "x".repeat(512)];
  for (const input of inputs) {
    const expected = expectedMatches(input);
    const label = labelFor(input);
    await search.set(input);
    await app.waitFor(async () => {
      const text = await search.get();
      if (text !== input) {
        throw new Error(
          `search text=${labelFor(String(text))} did not round-trip (expected ${label})`);
      }
      assertEq(await outerRowCount(app, proxyId), expected,
               `outer proxy rowCount() for input ${label}`);
      assertEq(await evalOn(app, emptyView.id, "visible"), expected === 0,
               `appManager.emptyView visible for input ${label}`);
    }, { timeout: 5000, interval: 100,
         description: `input ${label} to filter to ${expected} row(s)` });
    assertNoRegexWarnings(label);
  }

  await assertResponsive(app);
  await clearAppManagerSearch(app, search, proxyId, { preOuterRows });
  assertNoRegexWarnings('"" (clear)');
});

// --- App Manager (A11) — selecting a category filters the grid ---
//
// The filter is read as sourceModel.categoryFilter: object-scoped evaluate
// cannot reach AppManagerView's file-internal `d`. The selection shows
// through the cells' `highlighted` (ListView.isCurrentItem). Fixture A's
// manifest category is "testing", which AppsFilterProxy::categories()
// capitalizes; the expected count is computed from a snapshot of every
// outer row's category, mirroring filterAcceptsRow's capitalizeFirst.

test("app manager: selecting a category filters the grid", async (app) => {
  const proxyId = await openApplicationsWithProxy(app);
  const CATEGORY = "Testing";
  const capitalizeFirst = (s) => (s ? s.charAt(0).toUpperCase() + s.slice(1) : s);
  const categoryFilter = () => evalOn(app, proxyId, "sourceModel.categoryFilter");

  // Cells are re-found on every use: categoriesChanged can rebuild the
  // ListView's delegates.
  const findCategoryCell = (name) =>
    findByObjectName(app.inspector, `appManager.category.${name}`);
  const requireCell = async (name) => {
    const cell = await findCategoryCell(name);
    if (!cell) throw new Error(`appManager.category.${name} not in the QML tree`);
    return cell;
  };
  const clickCell = async (name) => evalOn(app, (await requireCell(name)).id, "clicked()");
  const cellHighlighted = async (name) => evalOn(app, (await requireCell(name)).id, "highlighted");

  // Normalize: "All" category and an empty search. The search is cleared
  // through the field so d.searchText stays in sync.
  const initialFilter = assertType(await categoryFilter(), "string", "sourceModel.categoryFilter");
  if (initialFilter !== "" && initialFilter !== "All") {
    await clickCell("All");
    await app.waitFor(async () => {
      assertEq(await categoryFilter(), "All", "sourceModel.categoryFilter");
    }, { timeout: 5000, interval: 100, description: 'category filter to normalize to "All"' });
  }
  if ((await evalOn(app, proxyId, "sourceModel.searchText")) !== "") {
    await (await searchFieldOn(app, "appManager.searchField")).set("");
    await app.waitFor(async () => {
      assertEq(await evalOn(app, proxyId, "sourceModel.searchText"), "", "sourceModel.searchText");
    }, { timeout: 5000, interval: 100, description: "search to normalize to empty" });
  }

  const cell = await requireFixtureA(app, "A11", () => requireCell(CATEGORY),
                                     `the "${CATEGORY}" category cell to exist`);
  if (cell === null) return;

  const preOuterRows = await outerRowCount(app, proxyId);
  const rowCategory = (i) => outerRowField(app, proxyId, i, APPS_ROLE.category);
  const snapshot = [];
  for (let i = 0; i < preOuterRows; i += 1) snapshot.push(await rowCategory(i));
  const expectedFiltered = snapshot.filter((c) => capitalizeFirst(c) === CATEGORY).length;
  if (expectedFiltered < 1) {
    throw new Error(
      `snapshot found 0 "${CATEGORY}"-category rows in ${preOuterRows} outer rows ` +
      `although the "${CATEGORY}" cell renders`);
  }

  await clickCell(CATEGORY);
  await app.waitFor(async () => {
    assertEq(await categoryFilter(), CATEGORY, "sourceModel.categoryFilter after the click");
    const outerRows = await outerRowCount(app, proxyId);
    assertEq(outerRows, expectedFiltered, `outer proxy rowCount() with the "${CATEGORY}" filter`);
    for (let i = 0; i < outerRows; i += 1) {
      assertEq(capitalizeFirst(await rowCategory(i)), CATEGORY, `filtered row ${i} category`);
    }
    assertEq(await cellHighlighted(CATEGORY), true, `"${CATEGORY}" cell highlighted`);
    assertEq(await cellHighlighted("All"), false, '"All" cell highlighted');
  }, { timeout: 5000, interval: 100,
       description: `the "${CATEGORY}" category to filter the grid to ${expectedFiltered} row(s)` });

  await clickCell("All");
  await app.waitFor(async () => {
    assertEq(await categoryFilter(), "All", 'sourceModel.categoryFilter after clicking "All"');
    assertEq(await outerRowCount(app, proxyId), preOuterRows,
             "outer proxy rowCount() after clearing the category");
    assertEq(await cellHighlighted("All"), true, '"All" cell highlighted after the reset');
  }, { timeout: 5000, interval: 100,
       description: 'the "All" category to restore the unfiltered grid' });
});

// --- App Manager (A12) — reload shows the loading state then settles ---
//
// The overlay's visible and the reload button's enabled both bind
// backend.appsLoading. Offline, the catalog fetch gives up after ~2 s, so the
// loading window is short: the phase gate polls all three observables in a
// fast loop and any one counts. A failed fetch never touches the model, so
// both counts must survive verbatim. The button is clicked by objectName:
// package_manager_ui renders its own "Reload".

test("app manager: reload shows the loading state then settles", async (app) => {
  const proxyId = await openApplicationsWithProxy(app);
  const reloadButton = await requireObject(app, "appManager.reloadButton");
  const overlay = await requireObject(app, "appManager.loadingOverlay");
  const appsLoading = () => evalOn(app, overlay.id, "backend.appsLoading");
  const overlayVisible = () => evalOn(app, overlay.id, "visible");
  const reloadEnabled = () => evalOn(app, reloadButton.id, "enabled");

  // No refresh may be in flight while the counts are recorded.
  await app.waitFor(async () => {
    assertEq(await appsLoading(), false, "backend.appsLoading");
    assertEq(await reloadEnabled(), true, "reload button enabled");
  }, { timeout: 30000, interval: 500, description: "no refresh to be in flight before the click" });
  const preOuterRows = await outerRowCount(app, proxyId);
  const preLocal = assertType(await evalOn(app, proxyId, "visibleCount"),
                              "number", "localAppsProxy.visibleCount");

  await invoke(app, reloadButton.id, "clicked", "clicking appManager.reloadButton");

  const phaseDeadline = Date.now() + 2000;
  let observed = null;
  for (;;) {
    if ((await overlayVisible()) === true) { observed = "loadingOverlay visible"; break; }
    if ((await appsLoading()) === true) { observed = "backend.appsLoading === true"; break; }
    if ((await reloadEnabled()) === false) { observed = "reload button disabled"; break; }
    if (Date.now() >= phaseDeadline) {
      throw new Error(
        "no loading observable within 2s of clicking reload (overlay hidden, " +
        "backend.appsLoading false, reload button enabled)");
    }
    await sleep(50);
  }
  console.log(`    loading phase observed via: ${observed}`);

  await app.waitFor(async () => {
    assertEq(await appsLoading(), false, "backend.appsLoading after reload");
    assertEq(await overlayVisible(), false, "appManager.loadingOverlay visible after reload");
    assertEq(await reloadEnabled(), true, "reload button enabled after reload");
  }, { timeout: 30000, interval: 250, description: "reload to settle" });

  // Single-shot reads: the counts must already be back the moment
  // appsLoading clears, and a retried wait would mask a transient drop.
  assertEq(await outerRowCount(app, proxyId), preOuterRows,
           "outer proxy rowCount() after the reload settled");
  assertEq(await evalOn(app, proxyId, "visibleCount"), preLocal,
           "localAppsProxy.visibleCount after the reload settled");
});

// --- App Manager — context-menu / Details-dialog helpers (A13–A15) ---------
//
// The welcome page also builds AppGridDelegate tiles, with contextMenuEnabled
// false and no detailsRequested wiring, and they precede the App Manager's in
// findByType order, so menus are selected by probing each AppContextMenu's
// delegate scope. Menu items are addressed through the menu's own
// count/itemAt: a tree-wide objectName find would hit another delegate's
// unopened menu. The AddApplicationDialog has no objectName and lives in the
// overlay QQuickWidget, so its texts are checked with a walk over contentItem.

// The outer-model roles the menu's appData consumes; kind picks the coercion.
const APPS_ROW_FIELDS = [
  ["name",          APPS_ROLE.name,          "string"],
  ["repositoryUrl", APPS_ROLE.repositoryUrl, "string"],
  ["displayName",   APPS_ROLE.displayName,   "string"],
  ["isInstalled",   APPS_ROLE.isInstalled,   "bool"],
  ["installStatus", APPS_ROLE.installStatus, "number"],
  ["installType",   APPS_ROLE.installType,   "string"],
  ["installStage",  APPS_ROLE.installStage,  "number"],
];

async function readOuterRow(app, proxyId, i) {
  const row = {};
  for (const [key, roleExpr, kind] of APPS_ROW_FIELDS) {
    row[key] = await outerRowField(app, proxyId, i, roleExpr, kind);
  }
  return row;
}

async function snapshotOuterRows(app, proxyId) {
  const rowCount = await outerRowCount(app, proxyId);
  const rows = [];
  for (let i = 0; i < rowCount; i += 1) rows.push(await readOuterRow(app, proxyId, i));
  return rows;
}

// Fixture A's installed row, or null.
async function findFixtureARow(app, proxyId) {
  const rowCount = await outerRowCount(app, proxyId);
  for (let i = 0; i < rowCount; i += 1) {
    const name = await outerRowField(app, proxyId, i, APPS_ROLE.name);
    if (name === FIXTURE_A.name) {
      const row = await readOuterRow(app, proxyId, i);
      return row.isInstalled === true ? row : null;
    }
  }
  return null;
}

// The AppContextMenu owned by the App Manager delegate rendering `row`, or
// null if none is live. Disabled menus are skipped.
async function findDelegateMenu(app, row) {
  const matches = await findByType(app, "AppContextMenu");
  if (matches.length === 0) throw new Error("no AppContextMenu instance in the QML tree");
  for (const m of matches) {
    const probe = await app.inspector.send("evaluate", {
      objectId: m.id,
      expression:
        "JSON.stringify({ name: String(d.nameText), " +
        "installed: d.isInstalled === true, " +
        "menuEnabled: root.contextMenuEnabled !== false })",
    });
    if (probe.error) {
      throw new Error(
        `delegate state does not resolve in AppContextMenu ${m.id}'s scope: ${probe.error}`);
    }
    const got = JSON.parse(probe.result);
    if (!got.menuEnabled) continue;
    if (got.name === row.name && got.installed === row.isInstalled) return m.id;
  }
  return null;
}

async function requireFixtureAMenu(app, row) {
  let menuId = null;
  await app.waitFor(async () => {
    menuId = await findDelegateMenu(app, row);
    if (menuId === null) {
      throw new Error(`no live delegate renders fixture A's installed row ("${row.name}")`);
    }
  }, { timeout: 10000, interval: 500,
       description: "fixture A's delegate (and its AppContextMenu) to exist" });
  return menuId;
}

// The delegate TapHandler's handler verbatim; waits for the menu to show.
async function openContextMenuFor(app, menuId, label) {
  await evalOn(app, menuId, "openFor(d.snapshot())");
  await app.waitFor(async () => {
    assertEq(await evalOn(app, menuId, "visible"), true,
             `AppContextMenu visible after openFor (${label})`);
  }, { timeout: 5000, interval: 100, description: "the context menu to open" });
}

// The appData the delegate handed the menu must equal its model row.
async function assertMenuAppData(app, menuId, row, label) {
  const appData = JSON.parse(await evalOn(app, menuId, "JSON.stringify(appData)"));
  for (const [key] of APPS_ROW_FIELDS) {
    assertEq(appData[key], row[key], `menu appData.${key} for the ${label} row`);
  }
}

async function closeContextMenu(app, menuId, label) {
  await evalOn(app, menuId, "close()");
  await app.waitFor(async () => {
    assertEq(await evalOn(app, menuId, "visible"), false, `AppContextMenu visible after close() (${label})`);
  }, { timeout: 5000, interval: 100, description: `the menu to close after ${label}` });
}

// Emits triggered() on this menu's item found by objectName among its own items.
async function triggerContextMenuItem(app, menuId, objectName) {
  const result = await evalOn(app, menuId, `(() => {
    for (let i = 0; i < count; i += 1) {
      const item = itemAt(i);
      if (!item || item.objectName !== ${JSON.stringify(objectName)}) continue;
      if (item.visible !== true) return "item not visible";
      item.triggered();
      return "triggered";
    }
    return "item not found";
  })()`);
  if (result !== "triggered") throw new Error(`triggering ${objectName} failed: ${result}`);
}

// Waits for the single AddApplicationDialog instance to be visible; returns its id.
async function waitForAddApplicationDialog(app) {
  let dialogId = null;
  await app.waitFor(async () => {
    dialogId = (await findByType(app, "AddApplicationDialog"))[0]?.id ?? null;
    if (dialogId === null) throw new Error("no AddApplicationDialog instance in the QML tree");
    assertEq(await evalOn(app, dialogId, "visible"), true, "AddApplicationDialog visible");
  }, { timeout: 10000, interval: 500, description: "the Add Application dialog to open" });
  return dialogId;
}

const missingDialogTexts = (app, dialogId, texts) =>
  missingTexts(app, dialogId, "contentItem", texts);

// Clicks addApplicationDialog.closeButton and waits for the dialog to hide.
async function closeAddApplicationDialog(app, dialogId) {
  const closeButton = await requireObject(app, "addApplicationDialog.closeButton");
  await invoke(app, closeButton.id, "clicked", "clicking addApplicationDialog.closeButton");
  await app.waitFor(async () => {
    assertEq(await evalOn(app, dialogId, "visible"), false,
             "AddApplicationDialog visible after the close click");
  }, { timeout: 5000, interval: 100, description: "the dialog to close" });
}

// Applications → fixture A's delegate menu → Details → dialog visible.
// Returns the dialog id, or null when the fixture-A precondition skipped.
async function openFixtureADetailsDialog(app, label) {
  const proxyId = await openApplicationsWithProxy(app);
  const fixtureRow = await requireFixtureA(app, label, async () => {
    const row = await findFixtureARow(app, proxyId);
    if (!row) throw new Error(`no installed row named "${FIXTURE_A.name}" in the outer model`);
    return row;
  }, "fixture A's installed row to appear in the model");
  if (fixtureRow === null) return null;

  const menuId = await requireFixtureAMenu(app, fixtureRow);
  await openContextMenuFor(app, menuId, "fixture A");
  await triggerContextMenuItem(app, menuId, "appContextMenu.details");
  // Direct signal emission bypasses the menu's auto-close.
  await closeContextMenu(app, menuId, "Details");
  return waitForAddApplicationDialog(app);
}

// --- App Manager (A13) — the context menu offers actions by install state ---
//
// Install state shows as item visibility: open/details iff installed;
// install iff not installed; uninstall iff installed && installType !==
// "embedded" && name !== "main_ui", enabled iff no install is in flight.
// The right-button binding is not covered: the inspector clicks left only.

test("app manager: context menu offers actions by install state", async (app) => {
  const proxyId = await openApplicationsWithProxy(app);

  const rows = await requireFixtureA(app, "A13", async () => {
    const snapshot = await snapshotOuterRows(app, proxyId);
    if (!snapshot.some((r) => r.name === FIXTURE_A.name && r.isInstalled === true)) {
      throw new Error(
        `no installed row named "${FIXTURE_A.name}" among ${snapshot.length} outer row(s)`);
    }
    return snapshot;
  }, "fixture A's installed row to appear in the model");
  if (rows === null) return;
  const installedRow = rows.find((r) => r.name === FIXTURE_A.name && r.isInstalled === true);
  const catalogRows = rows.filter((r) => r.isInstalled === false);

  const ITEM_NAMES = [
    "appContextMenu.open", "appContextMenu.details",
    "appContextMenu.install", "appContextMenu.uninstall",
  ];
  const menuItemStates = async (menuId) => {
    const states = JSON.parse(await evalOn(app, menuId, `(() => {
      const out = {};
      for (let i = 0; i < count; i += 1) {
        const item = itemAt(i);
        if (!item || !item.objectName) continue;
        out[item.objectName] = { visible: item.visible === true, enabled: item.enabled === true };
      }
      return JSON.stringify(out);
    })()`));
    for (const name of ITEM_NAMES) {
      if (!states[name]) {
        throw new Error(
          `menu item ${name} not among the menu's items ` +
          `(got: ${Object.keys(states).join(", ") || "none"})`);
      }
    }
    return states;
  };
  const assertItem = (states, name, expected, label) => {
    for (const [prop, want] of Object.entries(expected)) {
      assertEq(states[name][prop], want, `${name} ${prop} for the ${label} row`);
    }
  };

  const installedMenuId = await requireFixtureAMenu(app, installedRow);
  await openContextMenuFor(app, installedMenuId, "installed");
  await app.waitFor(async () => {
    await assertMenuAppData(app, installedMenuId, installedRow, "installed");
    const states = await menuItemStates(installedMenuId);
    assertItem(states, "appContextMenu.open",    { visible: true },  "installed");
    assertItem(states, "appContextMenu.details", { visible: true },  "installed");
    assertItem(states, "appContextMenu.install", { visible: false }, "installed");
    assertItem(states, "appContextMenu.uninstall", { visible: true, enabled: true }, "installed");
  }, { timeout: 5000, interval: 100,
       description: "the installed row's menu to offer open/details/uninstall" });
  await closeContextMenu(app, installedMenuId, "the installed row");

  // First not-installed row with a live delegate.
  let catalogRow = null;
  let catalogMenuId = null;
  for (const row of catalogRows) {
    const id = await findDelegateMenu(app, row);
    if (id !== null) { catalogRow = row; catalogMenuId = id; break; }
  }
  if (catalogMenuId === null) {
    console.log(
      `    SKIP: A13 catalog-only half — ${catalogRows.length} of ${rows.length} outer ` +
      "row(s) are not installed, but none has a live delegate");
    return;
  }
  await openContextMenuFor(app, catalogMenuId, "catalog-only");
  await app.waitFor(async () => {
    await assertMenuAppData(app, catalogMenuId, catalogRow, "catalog-only");
    const states = await menuItemStates(catalogMenuId);
    assertItem(states, "appContextMenu.install",   { visible: true },  "catalog-only");
    assertItem(states, "appContextMenu.open",      { visible: false }, "catalog-only");
    assertItem(states, "appContextMenu.details",   { visible: false }, "catalog-only");
    assertItem(states, "appContextMenu.uninstall", { visible: false }, "catalog-only");
  }, { timeout: 5000, interval: 100,
       description: "the catalog-only row's menu to offer install alone" });
  await closeContextMenu(app, catalogMenuId, "the catalog-only row");
});

// --- App Manager (A14) — Details opens the Add Application dialog ---

test("app manager: context menu Details opens the Add Application dialog", async (app) => {
  const dialogId = await openFixtureADetailsDialog(app, "A14");
  if (dialogId === null) return;

  await app.waitFor(async () => {
    const missing = await missingDialogTexts(app, dialogId, [
      "Add Application", FIXTURE_A.displayName, "Description", "Required Packages",
    ]);
    if (missing.length > 0) throw new Error(`dialog texts missing: ${missing.join(", ")}`);
  }, { timeout: 5000, interval: 250, description: "the dialog's fixed texts to render" });

  assertEq(await evalOn(app, dialogId, "installStage"), 0,
           "dialog.installStage (0 = InstallStage.None)");

  await closeAddApplicationDialog(app, dialogId);
});

// --- App Manager (A15) — dialog wording for an already-installed app ---
//
// Fixture A is a catalog-less user install, so actionMode resolves to
// "launch" and the primary button reads "Launch"; "%1 will be installed."
// comes only from the "install" footer arm. Uninstall keys on d.canUninstall,
// all true for fixture A.

test("app manager: dialog wording for an already-installed app", async (app) => {
  const dialogId = await openFixtureADetailsDialog(app, "A15");
  if (dialogId === null) return;

  const primaryButton = await requireObject(app, "addApplicationDialog.primaryButton");
  await app.waitFor(async () => {
    assertEq(await evalOn(app, primaryButton.id, "text"), "Launch", "primaryButton text");
  }, { timeout: 5000, interval: 100, description: 'the primary button to read "Launch"' });

  const missing = await missingDialogTexts(app, dialogId, ["will be installed."]);
  if (missing.length !== 1) {
    throw new Error('"will be installed." found in the dialog (expected absent)');
  }

  const uninstallButton = await requireObject(app, "addApplicationDialog.uninstallButton");
  await app.waitFor(async () => {
    assertEq(await evalOn(app, uninstallButton.id, "visible"), true, "uninstallButton visible");
    assertEq(await evalOn(app, uninstallButton.id, "enabled"), true, "uninstallButton enabled");
  }, { timeout: 5000, interval: 100,
       description: "the Uninstall button to be visible and enabled" });

  await closeAddApplicationDialog(app, dialogId);
});

// --- Settings helpers --------------------------------------------------------

async function openSettingsEntry(app, entry, expectedTexts = []) {
  await app.click("Settings", sidebarSection);
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Apps Inspector", "Module Inspector"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
  await app.click(entry, { type: "LogosItemDelegate" });
  if (expectedTexts.length > 0) {
    await app.waitFor(
      async () => { await app.expectTexts(expectedTexts); },
      { timeout: 10000, interval: 500, description: `${entry} to become active` }
    );
  }
}

// --- Settings (A16) — Dashboard shows version, build type and commits ---
//
// Every expectation is derived from the backend. DashboardView has no
// objectNames, so the gates run as one JS walk scoped to the view, which also
// keeps the sidebar footer's build-type token out of the never-both check.
// Commit rows are the children of the "Commits" column holding exactly two
// text nodes: name, then commit.

test("settings: Dashboard shows version, build type and commit list", async (app) => {
  await openSettingsEntry(app, "Dashboard");

  // DashboardView is instantiated eagerly; only `visible` proves it is selected.
  let dashboard = null;
  await app.waitFor(async () => {
    dashboard = (await findByType(app, "DashboardView"))[0] || null;
    if (!dashboard) throw new Error("no DashboardView instance in the QML tree");
    assertEq(await evalOn(app, dashboard.id, "visible"), true, "DashboardView visible");
  }, { timeout: 10000, interval: 500, description: "Dashboard view to become visible" });

  // An empty buildVersion is valid (dirty local builds bake "").
  const buildVersion = assertType(await evalOn(app, dashboard.id, "backend.buildVersion"),
                                  "string", "backend.buildVersion");
  const commitCount = await evalOn(app, dashboard.id, "backend.buildCommits.length");
  if (typeof commitCount !== "number" || commitCount < 1) {
    throw new Error(`backend.buildCommits.length=${JSON.stringify(commitCount)} (expected ≥ 1)`);
  }
  const firstName = await evalOn(app, dashboard.id, "backend.buildCommits[0].name");
  const firstCommit = await evalOn(app, dashboard.id, "backend.buildCommits[0].commit");
  if (typeof firstName !== "string" || firstName.length === 0 ||
      typeof firstCommit !== "string" || firstCommit.length === 0) {
    throw new Error(
      `backend.buildCommits[0] name=${JSON.stringify(firstName)} ` +
      `commit=${JSON.stringify(firstCommit)} (expected non-empty strings)`);
  }
  const isPortable = assertType(await evalOn(app, dashboard.id, "backend.isPortableBuild"),
                                "boolean", "backend.isPortableBuild");

  const snapshotDashboard = async () => JSON.parse(await evalOn(app, dashboard.id, `(() => {
    const out = {
      hasVersion: false, versionLabelVisible: false, hasCommits: false,
      hasPortable: false, hasDev: false, rows: null,
    };
    const walk = (node) => {
      if (!node) return;
      if (node.text === ${JSON.stringify(buildVersion)}) out.hasVersion = true;
      // Item.visible is effective visibility, so it tracks the hidden row.
      if (node.text === "Version" && node.visible === true) out.versionLabelVisible = true;
      if (node.text === "Portable build") out.hasPortable = true;
      if (node.text === "Dev build") out.hasDev = true;
      const kids = node.children;
      if (!kids || typeof kids.length !== "number") return;
      let hasHeading = false;
      for (let i = 0; i < kids.length; i += 1) {
        if (kids[i] && kids[i].text === "Commits") hasHeading = true;
      }
      if (hasHeading) {
        out.hasCommits = true;
        out.rows = [];
        for (let i = 0; i < kids.length; i += 1) {
          const k = kids[i];
          const two = k && k.children && k.children.length === 2 ? k.children : null;
          if (two && typeof two[0].text === "string" && typeof two[1].text === "string") {
            out.rows.push({ name: two[0].text, commit: two[1].text });
          }
        }
        return;
      }
      for (let i = 0; i < kids.length; i += 1) walk(kids[i]);
    };
    walk(this);
    return JSON.stringify(out);
  })()`));

  const expectedType = isPortable ? "Portable build" : "Dev build";
  const otherType = isPortable ? "Dev build" : "Portable build";
  await app.waitFor(async () => {
    const snap = await snapshotDashboard();
    if (buildVersion.length > 0) {
      if (snap.hasVersion !== true) {
        throw new Error(
          `no text equal to buildVersion=${JSON.stringify(buildVersion)} in the Dashboard view`);
      }
    } else if (snap.versionLabelVisible !== false) {
      throw new Error('the "Version" row is visible although backend.buildVersion is empty');
    }
    if (snap.hasCommits !== true) {
      throw new Error('"Commits" heading not found in the Dashboard view');
    }
    if (!Array.isArray(snap.rows) || snap.rows.length !== commitCount) {
      throw new Error(
        `${Array.isArray(snap.rows) ? snap.rows.length : "no"} commit rows rendered ` +
        `(expected backend.buildCommits.length=${commitCount})`);
    }
    if (snap.rows[0].name !== firstName || snap.rows[0].commit !== firstCommit) {
      throw new Error(
        `first commit row=${JSON.stringify(snap.rows[0])} (expected ` +
        `name=${JSON.stringify(firstName)} commit=${JSON.stringify(firstCommit)})`);
    }
    const hasExpected = isPortable ? snap.hasPortable : snap.hasDev;
    const hasOther = isPortable ? snap.hasDev : snap.hasPortable;
    if (hasExpected !== true || hasOther !== false) {
      throw new Error(
        `build-type texts: "${expectedType}"=${hasExpected} "${otherType}"=${hasOther} ` +
        `(isPortableBuild=${isPortable}; expected the matching one alone)`);
    }
  }, { timeout: 10000, interval: 500,
       description: "Dashboard to render version, build type and commit rows" });
});

// --- Settings (A17) — Apps Inspector search filters the table ---
//
// settings.searchField is SettingsView's page-level LogosSearchBar, shared by
// both inspectors and reset on every section switch. Counts come from
// appsInspector.table's ModulesFilterProxy; the expected "package" count is
// derived from a snapshot of the roles the proxy matches. The "zzz" absence
// walk is first proven against the rendered badges with the search empty,
// so an empty result cannot pass vacuously.

// ModuleInstanceRoles (app/interfaces/BasecampModelRoles.h) the search matches.
const MODULE_SEARCH_ROLES = {
  name: "Qt.UserRole + 1",
  label: "Qt.UserRole + 2",
  description: "Qt.UserRole + 3",
  version: "Qt.UserRole + 6",
  statusText: "Qt.UserRole + 12",
};

test("apps inspector: search filters the table", async (app) => {
  await openAppsInspector(app);

  // AppsInspectorView is instantiated eagerly; only `visible` proves it is selected.
  const view = await requireObject(app, "appsInspectorView");
  await app.waitFor(async () => {
    assertEq(await evalOn(app, view.id, "visible"), true, "appsInspectorView visible");
  }, { timeout: 10000, interval: 500, description: "Apps Inspector view to become visible" });

  const table = await requireObject(app, "appsInspector.table");
  const visibleCount = () => evalOn(app, table.id, "model.visibleCount");
  const search = await searchFieldOn(app, "settings.searchField");
  await search.normalize();

  let initialCount = 0;
  await app.waitFor(async () => {
    const total = await evalOn(app, table.id, "model.totalCount");
    if (typeof total !== "number" || total < 1) {
      throw new Error(`model.totalCount=${JSON.stringify(total)} (expected ≥ 1)`);
    }
    assertEq(await visibleCount(), total, "model.visibleCount with an empty search");
    initialCount = total;
  }, { timeout: 10000, interval: 500, description: "apps table to populate" });

  const rows = [];
  for (let i = 0; i < initialCount; i += 1) {
    const row = {};
    for (const [key, roleExpr] of Object.entries(MODULE_SEARCH_ROLES)) {
      row[key] = await evalOn(
        app, table.id, `String(model.data(model.index(${i}, 0), ${roleExpr}) || "")`);
    }
    rows.push(row);
  }
  const expectedMatches = rows.filter((row) =>
    Object.values(row).some((v) => v.toLowerCase().includes("package"))).length;
  if (expectedMatches < 1) {
    throw new Error(
      'no snapshot row matches "package" although package_manager_ui is always ' +
      `installed (rows=${JSON.stringify(rows)})`);
  }

  // Rendered "Loaded" / "Not loaded" badge texts under the table.
  const collectStatusTexts = async () => JSON.parse(await evalOn(app, table.id, `(() => {
    const found = [];
    const walk = (node) => {
      if (!node) return;
      if (typeof node.text === "string") {
        const t = node.text.replace(/[()]/g, "").trim().toLowerCase();
        if (t === "loaded" || t === "not loaded") found.push(node.text);
      }
      const kids = node.children;
      if (!kids || typeof kids.length !== "number") return;
      for (let i = 0; i < kids.length; i += 1) walk(kids[i]);
    };
    walk(this);
    return JSON.stringify(found);
  })()`));

  // Positive control: other rows carry "Main UI" / dependency wording, hence at-least.
  const badgeRows = rows.filter((row) => {
    const t = row.statusText.trim().toLowerCase();
    return t === "loaded" || t === "not loaded";
  }).length;
  if (badgeRows >= 1) {
    await app.waitFor(async () => {
      const found = await collectStatusTexts();
      if (found.length < badgeRows) {
        throw new Error(
          `status-text walk found ${found.length} badge texts (${JSON.stringify(found)}) ` +
          `with the search empty, but the snapshot has ${badgeRows} badge rows`);
      }
    }, { timeout: 5000, interval: 250, description: "status-text walk to see the rendered badges" });
  }

  await search.set("package");
  await app.waitFor(async () => {
    await search.expectText("package");
    assertEq(await visibleCount(), expectedMatches,
             `model.visibleCount for "package" (of ${initialCount} rows)`);
  }, { timeout: 5000, interval: 250,
       description: '"package" search to keep exactly the matching rows' });

  // Filtered-out rows destroy their delegates; no badge text may remain.
  await search.set("zzz");
  await app.waitFor(async () => {
    await search.expectText("zzz");
    assertEq(await visibleCount(), 0, 'model.visibleCount for "zzz"');
    const leftovers = await collectStatusTexts();
    if (leftovers.length !== 0) {
      throw new Error(
        `status texts still rendered with zero matches: ${JSON.stringify(leftovers)}`);
    }
  }, { timeout: 5000, interval: 250, description: '"zzz" search to empty the table' });

  await search.set("");
  await app.waitFor(async () => {
    await search.expectText("");
    assertEq(await visibleCount(), initialCount, "model.visibleCount after clearing the search");
  }, { timeout: 5000, interval: 250, description: "cleared search to restore every row" });
});

// --- Package Manager ---
//
// PMUI is no longer launched from the sidebar app launcher (filtered out
// in UIPluginManager::launcherApps); it now lives behind the dedicated
// "Package Manager" sidebar section button, which lazy-loads PMUI into
// MainContainer's QStackedWidget slot 2 on first click.
test("package_manager_ui: section click loads PMUI's own QML", async (app) => {
  // This used to assert ["Reload"], which is NOT evidence of anything: the
  // Reload button is rendered by basecamp's OWN InspectorPanelHeader.qml:85
  // and AppManagerPanelHeader.qml:71, inside ContentViews.qml, whose
  // StackLayout instantiates every page whether or not it is visible. So
  // "Reload" was in the object tree from startup, and the assertion held even
  // though PMUI had never been loaded, its dylib had never been mapped and
  // ui-host had never been spawned — see sidebarSection above.
  //
  // These two strings come from PMUI itself and from nowhere else:
  //   "Manage your plugins and packages." — logos-package-manager-ui
  //                                         src/qml/Panels/HeaderBar.qml
  //   "Types"                             — src/qml/Panels/CategorySidebar.qml
  // Neither appears anywhere in basecamp's own QML, so neither can be
  // satisfied unless PMUI's QML is live in MainContainer's stack slot 2 —
  // which requires the plugin to have loaded and its ui-host to be up.
  //
  // 45s, not the default 10s: the load spawns a ui-host process, waits on its
  // ready handshake (PluginLoader gives that 30s) and only then compiles the
  // QML.
  await openPlugin(app, "Package Manager",
                   ["Manage your plugins and packages.", "Types"],
                   { ...sidebarSection, timeout: 45000 });

  // The placeholder QLabel is removed from the stack the moment PMUI's real
  // widget is inserted (MainContainer's pluginWindowRequested intercept), so
  // its absence is a second, independent witness that the swap happened —
  // and it is exactly the object the old bare click was hitting.
  const stillPlaceholder = await app.findByProperty("text", "Loading Package Manager…");
  if ((stillPlaceholder.matches || []).length > 0) {
    throw new Error("PMUI placeholder is still in the stack — the real widget never arrived");
  }
});

// --- Host-services grant ---
//
// The test above proves PMUI's QML is LIVE. It says nothing about whether PMUI
// can actually TALK to anything, and that gap is why this suite certified a
// build in which capability_module had been denied its token_registry /
// token_delivery grant: ui-host's every call came back
// "ModuleProxy: rejecting unauthorized call" (34 of them), PMUI rendered its
// chrome over an empty backend, and all 16 tests still passed.
//
// This one asserts the opposite direction — an outcome only a SUCCESSFUL
// privileged operation can produce. See tests/host-services-assert.mjs.
test("host-services: package_manager_ui completes a capability-gated call chain", async (app) => {
  await assertHostServicesGrantReached(app, { timeout: 90000, log: console.log });
});

test("settings: shows Dashboard, Apps Inspector, Module Inspector entries", async (app) => {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Apps Inspector", "Module Inspector"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
});

test("settings: clicking Dashboard renders the Dashboard view", async (app) => {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Apps Inspector", "Module Inspector"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
  await app.click("Dashboard", { type: "LogosItemDelegate" });
  await app.waitFor(
    async () => { await app.expectTexts(["Commits"]); },
    { timeout: 10000, interval: 500, description: "Dashboard view to render" }
  );
});

// --- Inspectors (Apps + Module) ---
//
// Regression test: navigating to Module Inspector must show auto-loaded core
// modules (package_manager, capability_module) with a "Loaded" status badge,
// not "Not loaded". The bug we hit was that
// MainUIBackend::refreshCoreModules() called logos_core_refresh_modules(),
// which re-ran ModuleRegistry::discoverInstalledModules() and wiped the
// `loaded` flag of every module via `m_modules.insert(qName, freshInfo)`.
// The whole list then rendered as Not loaded with no CPU/Mem stats.
//
// The old "Settings → Modules" sub-tab (with UI Modules + Core Modules
// nested tabs) was split into two top-level Settings sections:
// "Apps Inspector" (UI plugins) and "Module Inspector" (core modules,
// with live CPU/memory + Interface drilldown).
const openAppsInspector = (app) =>
  openSettingsEntry(app, "Apps Inspector", ["UI plugins available in this installation."]);

const openModuleInspector = (app) =>
  openSettingsEntry(app, "Module Inspector",
                    ["Core modules known to the runtime, with live resource usage."]);

test("apps inspector: shows installed UI plugins", async (app) => {
  await openAppsInspector(app);
  await app.waitFor(
    // Asserting ["Package Manager"] alone is vacuous: the sidebar's own
    // section button carries exactly that text, so it holds with the table
    // completely empty. Assert the RAW module name, which
    // AppsInspectorView.qml:187-192 renders in the row
    // (`visible: rowItem.label !== rowItem.name`). Measured on a fresh app:
    //   findByProperty(text,"Package Manager")   -> 2  (SidebarCircleButton, row)
    //   findByProperty(text,"package_manager_ui")-> 1  (the row's LogosText)
    //   findByProperty(text,"Main UI")           -> 0  (not an installed plugin)
    async () => { await app.expectTexts(["package_manager_ui"]); },
    { timeout: 10000, interval: 500, description: "Apps Inspector list to populate" }
  );
});

test("module inspector: auto-loaded modules show as Loaded with Unload action", async (app) => {
  await openModuleInspector(app);

  // Wait for the module list to populate.
  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager"]); },
    { timeout: 10000, interval: 500, description: "Module Inspector list to populate" }
  );

  // ModuleStatusBadge renders "Loaded" for loaded modules and "Not loaded"
  // for unloaded ones (no parens). ModuleRowActions renders "Unload" (or
  // "Load"). With the refreshCoreModules bug, every module showed
  // "Not loaded" and only "Load" buttons appeared, so neither "Loaded" nor
  // "Unload" appeared anywhere in the Module Inspector table.
  await app.waitFor(
    async () => { await app.expectTexts(["Loaded", "Unload"]); },
    { timeout: 10000, interval: 500, description: "loaded status and Unload button to appear" }
  );
});

test("module inspector: loaded modules render CPU and memory stats", async (app) => {
  await openModuleInspector(app);

  // Wait for at least one loaded plugin to appear.
  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager", "Loaded"]); },
    { timeout: 10000, interval: 500, description: "loaded plugins to appear" }
  );

  // Live stats update every 2s. The current Module Inspector cell format
  // is "<num>%" for CPU and "<num> MB" for memory (the "CPU:" / "Mem:"
  // prefixes moved to the column headers). Verify the two column headers
  // are present AND that at least one numeric-with-unit value has rendered
  // — proof that the stats poll is actually populating rows.
  await app.waitFor(
    async () => { await app.expectTexts(["CPU", "Memory"]); },
    { timeout: 15000, interval: 500, description: "CPU and Memory column headers to render" }
  );
  await app.waitFor(
    async () => {
      const tree = await app.getTree({ depth: 40 });
      const treeStr = JSON.stringify(tree);
      // Match the actual delegate output: <digit>% and <digit> MB.
      if (!/\d\.\d%/.test(treeStr)) {
        throw new Error("No CPU percentage rendered for loaded modules");
      }
      if (!/\d\.\d MB/.test(treeStr)) {
        throw new Error("No memory-in-MB rendered for loaded modules");
      }
    },
    { timeout: 15000, interval: 2000, description: "CPU % and Memory MB values to appear" }
  );
});

test("module inspector: leaving and returning preserves loaded state", async (app) => {
  // Navigate to Settings → Module Inspector and wait for loaded modules.
  await openModuleInspector(app);

  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager", "Loaded"]); },
    { timeout: 10000, interval: 500, description: "Module Inspector to show loaded modules" }
  );

  // Navigate away to a different top-level section (Applications).
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Install and manage applications."]); },
    { timeout: 10000, interval: 500, description: "Applications view to render" }
  );

  // Navigate back to Settings → Module Inspector.
  await openModuleInspector(app);

  // The previously-loaded modules must still show as "Loaded" with the Unload action.
  await app.waitFor(
    async () => { await app.expectTexts(["Package Manager", "Loaded", "Unload"]); },
    { timeout: 10000, interval: 500, description: "loaded state to be preserved after returning" }
  );
});

// --- Sidebar: sequential section opening ---
//
// Regression guard: opening multiple sidebar sections one after another
// must not crash, hang, or leave the sidebar in an inconsistent state.
// Each section is opened via its sidebar button, we wait for expected
// content to render, then move on to the next. Finally we verify each
// section is still reachable by switching back to it.
//
// (Previously this iterated launcher-installed plugins, but PMUI is now
// the only one and it lives behind a section button rather than the
// launcher, so this is now a section walk.)
test("sidebar: open multiple sections sequentially without failure", async (app) => {
  const sections = [
    { name: "Applications",    expect: ["Install and manage applications."] },
    // Was ["Reload"] — rendered by basecamp's own panel headers regardless of
    // PMUI, so this leg of the walk asserted nothing. PMUI's header subtitle
    // can only come from PMUI. Every click here goes through sidebarSection
    // for the reason documented at the top of this file.
    { name: "Package Manager", expect: ["Manage your plugins and packages."] },
    { name: "Settings",        expect: ["Manage modules, apps and dashboards.", "Sections"] },
  ];

  for (const section of sections) {
    await openPlugin(app, section.name, section.expect,
                     { ...sidebarSection, timeout: 45000 });
  }

  for (const section of sections) {
    await app.click(section.name, sidebarSection);
    await app.waitFor(
      async () => { await app.expectTexts(section.expect); },
      { timeout: 10000, interval: 500, description: `"${section.name}" still accessible` }
    );
  }
});

// --- App Manager ---
test("app manager: panel + categories sidebar render on first open", async (app) => {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Apps", "Categories", "All"]); },
    { timeout: 15000, interval: 500, description: "App Manager content" }
  );
});

// ---------------------------------------------------------------------------
// Repositories view — disable vs remove semantics
// ---------------------------------------------------------------------------
//
// Regression guards for two related bugs:
//   (a) toggling the default off silently REMOVED it from the list,
//       indistinguishable from a real deletion — disable and remove
//       collapsed into a single state at the library layer.
//   (b) no coverage for the correct semantics: disable keeps the row with
//       enabled=false, remove drops it entirely, re-add restores it.
//
// The library now has independent defaultDisabled / defaultRemoved flags;
// these tests pin the observable behavior end-to-end through the coordinator.
async function openRepositoriesView(app) {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Sections", "Package Repositories"]); },
    { timeout: 10000, interval: 500, description: "Settings sections" }
  );
  await app.click("Package Repositories", { exact: true });
  await app.waitFor(
    async () => { await app.expectTexts(["Add a repository", "Default"]); },
    { timeout: 10000, interval: 500, description: "Repositories view content" }
  );
  const anchor = await app.findByProperty("text", "Package Repositories");
  if (!anchor.matches || anchor.matches.length === 0) {
    throw new Error("Package Repositories heading not found");
  }
  return anchor.matches[0].id;
}

async function isDefaultInList(app, anchorId) {
  return (await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) if (rs[i].isDefault) return true;
      return false;
    })()`,
  })).result === true;
}

async function isDefaultEnabled(app, anchorId) {
  return (await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) if (rs[i].isDefault) return rs[i].enabled !== false;
      return null;
    })()`,
  })).result;
}

test("repositories: disabling default keeps it in the list", async (app) => {
  const anchorId = await openRepositoriesView(app);

  const initialCount = (await app.inspector.send("evaluate", {
    objectId: anchorId, expression: "backend.repositories.length",
  })).result;
  if (typeof initialCount !== "number" || initialCount < 1) {
    throw new Error(`backend.repositories.length=${initialCount} (expected ≥ 1)`);
  }

  // Bypass the LogosSwitch click path (coordinate hit-testing on offscreen
  // is fragile). The bug lived in the library round-trip
  // (setRepositoryEnabled → getRepositories), which is exactly what this
  // flow exercises via `backend`.
  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) {
        if (rs[i].isDefault) { backend.setRepositoryEnabled(rs[i].url, false); return; }
      }
    })()`,
  });

  await app.waitFor(async () => {
    const count = (await app.inspector.send("evaluate", {
      objectId: anchorId, expression: "backend.repositories.length",
    })).result;
    if (count !== initialCount) {
      throw new Error(
        `default repo dropped from list after disable: length=${count}, ` +
        `initial=${initialCount}. This is the bug: disable must keep the row.`);
    }
    if ((await isDefaultEnabled(app, anchorId)) !== false) {
      throw new Error("default still shows enabled=true after disable");
    }
  }, { timeout: 5000, interval: 250, description: "disabled default to stay in list" });

  // Restore so subsequent tests start clean.
  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) {
        if (rs[i].isDefault) { backend.setRepositoryEnabled(rs[i].url, true); return; }
      }
    })()`,
  });
  await app.waitFor(async () => {
    if ((await isDefaultEnabled(app, anchorId)) !== true) {
      throw new Error("default did not re-enable");
    }
  }, { timeout: 5000, interval: 250, description: "re-enable to settle" });
});

// Full round trip: remove → re-add → assert restored.
//
// Re-add of a defaultRemoved URL goes through RepositoryRegistry::addRepository,
// which HTTPs-fetches the default's logos-repo.json before flipping the flag
// (downloader package_downloader_lib.cpp:485-514). Sandboxed nix builds /
// offscreen CI have no network, so this test is skipped there — the remove-only
// test below still covers the remove half.
//
// Runs FIRST in the pair (before the remove-only test) so that on local the
// state is restored between them: this test ends with the default present,
// then the remove-only test cleanly removes it. On offscreen this one skips,
// and the remove-only test runs against the initial default-present state.
test("repositories: removing default drops it from list; re-adding restores it", async (app) => {
  const anchorId = await openRepositoriesView(app);

  if (!(await isDefaultInList(app, anchorId))) {
    throw new Error("default not present at test start");
  }
  const defaultUrl = (await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) if (rs[i].isDefault) return rs[i].url;
      return "";
    })()`,
  })).result;
  if (typeof defaultUrl !== "string" || defaultUrl.length === 0) {
    throw new Error("could not read default url");
  }

  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `backend.removeRepository(${JSON.stringify(defaultUrl)})`,
  });
  await app.waitFor(async () => {
    if (await isDefaultInList(app, anchorId)) {
      throw new Error("default still in list after remove — remove must drop the row");
    }
  }, { timeout: 5000, interval: 250, description: "default row to disappear" });

  // Re-add by URL — flips both defaultRemoved and defaultDisabled off.
  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `backend.addRepository(${JSON.stringify(defaultUrl)})`,
  });
  await app.waitFor(async () => {
    if (!(await isDefaultInList(app, anchorId))) {
      throw new Error("default did not come back after addRepository");
    }
    if ((await isDefaultEnabled(app, anchorId)) !== true) {
      throw new Error("re-added default is not enabled");
    }
  }, { timeout: 5000, interval: 250, description: "default row to return enabled" });
}, { skip: ["offscreen"] });

// Remove is a pure flag flip on the downloader side (defaultRemoved=true),
// no network — safe to assert everywhere including offscreen CI. Runs
// AFTER the full round-trip test above so state ordering works on local:
//   local:      full test (ends default-present) → this test (removes) → done
//   offscreen:  full test skipped                → this test (removes) → done
test("repositories: removing default drops it from list", async (app) => {
  const anchorId = await openRepositoriesView(app);

  if (!(await isDefaultInList(app, anchorId))) {
    throw new Error("default not present at test start");
  }
  const defaultUrl = (await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `(function() {
      var rs = backend.repositories;
      for (var i = 0; i < rs.length; ++i) if (rs[i].isDefault) return rs[i].url;
      return "";
    })()`,
  })).result;
  if (typeof defaultUrl !== "string" || defaultUrl.length === 0) {
    throw new Error("could not read default url");
  }

  await app.inspector.send("evaluate", {
    objectId: anchorId,
    expression: `backend.removeRepository(${JSON.stringify(defaultUrl)})`,
  });
  await app.waitFor(async () => {
    if (await isDefaultInList(app, anchorId)) {
      throw new Error("default still in list after remove — remove must drop the row");
    }
  }, { timeout: 5000, interval: 250, description: "default row to disappear" });
});

// ---------------------------------------------------------------------------
// ShortcutBridge end-to-end
// ---------------------------------------------------------------------------
test("shortcut bridge: ⌘K in AppManager focuses the search bar", async (app) => {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Apps", "Categories"]); },
    { timeout: 15000, interval: 500, description: "App Manager visible" }
  );

  // QML declares sequence: "Ctrl+K". QShortcut.key stringifies as NativeText
  // — "⌘K" on macOS, "Ctrl+K" elsewhere — so try both.
  const keyForms = ["Ctrl+K", "⌘K"];
  let mirrors = [];
  for (const value of keyForms) {
    const mirrorSearch = await app.inspector.send("findByProperty", {
      property: "key", value,
    });
    mirrors = (mirrorSearch.matches ?? []).filter(
      m => (m.type ?? "").startsWith("QShortcut")
    );
    if (mirrors.length > 0) break;
  }
  if (mirrors.length === 0) {
    throw new Error(
      `ShortcutBridge did not mirror Ctrl+K on the host (tried ${keyForms.join(", ")})`
    );
  }

  const activated = await app.inspector.send("callMethod", {
    objectId: mirrors[0].id, method: "activated",
  });
  if (activated.error) {
    throw new Error(`callMethod(activated) failed: ${activated.error}`);
  }

  await app.waitFor(async () => {
    const bar = await app.inspector.send("findByProperty", {
      property: "placeholderText", value: "Search apps…",
    });
    const bid = bar.matches?.[0]?.id;
    if (!bid) throw new Error("Search apps… bar not found");
    const evalR = await app.inspector.send("evaluate", {
      objectId: bid, expression: "textInput.activeFocus",
    });
    if (evalR.result !== true)
      throw new Error(`textInput.activeFocus = ${evalR.result}, expected true`);
  }, { timeout: 3000, interval: 200, description: "search bar to focus" });
});

// --- App Manager "Local" section ---
//
// Two invariants for the synthetic "Local" repo bucket in AppManagerView:
//   (a) matchLocalOnly on AppsFilterProxy picks up exactly the rows in
//       AppsModel that have an empty repositoryUrl;
//   (b) the "Local" section header renders in the grid iff local rows exist.
//
// The test inspects the live QML tree — no fixture-seeding, so it verifies
// the invariant against whatever the harness has installed. If the harness
// has zero local packages the test short-circuits ("ok, no local rows"),
// which is fine: what we're guarding against is a wrong / silent-broken
// mapping, not fixture presence.
//
// Anchor: AppManagerView declares `AppsFilterProxy { objectName:
// "appManager.localAppsProxy"; matchLocalOnly: true; sourceModel:
// root.appsProxy }`. From that anchor we can reach both the proxy's own
// visibleCount and the underlying AppsModel via sourceModel.sourceModel.

async function findLocalAppsProxy(app) {
  const res = await app.findByProperty("objectName", "appManager.localAppsProxy");
  if (!res.matches || res.matches.length === 0) return null;
  return res.matches[0].id;
}

// Evaluate expects a primitive result — object literals come back as
// opaque "<QJSValue>". So we do one call per property. Small wrapper
// keeps call sites readable.
async function evalOn(app, objectId, expression) {
  const res = await app.inspector.send("evaluate", { objectId, expression });
  if (res.error) throw new Error(`evaluate("${expression}") failed: ${res.error}`);
  return res.result;
}

test("app manager: localAppsProxy is wired as a matchLocalOnly filter", async (app) => {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Apps", "Categories"]); },
    { timeout: 15000, interval: 500, description: "App Manager to render" }
  );

  const proxyId = await findLocalAppsProxy(app);
  if (proxyId === null) {
    throw new Error("appManager.localAppsProxy not found in QML tree");
  }

  // Guards the wiring: without matchLocalOnly=true the section would
  // silently absorb every catalog row when repositoryUrlFilter is empty.
  // Without a sourceModel it would report 0 forever.
  const matchLocalOnly = await evalOn(app, proxyId, "matchLocalOnly");
  if (matchLocalOnly !== true) {
    throw new Error(`matchLocalOnly=${matchLocalOnly} (expected true)`);
  }
  const excludeMainUi = await evalOn(app, proxyId, "excludeMainUi");
  if (excludeMainUi !== false) {
    throw new Error(`excludeMainUi=${excludeMainUi} (expected false — Local shows all)`);
  }
  const hasSourceModel = await evalOn(app, proxyId, "!!sourceModel");
  if (!hasSourceModel) {
    throw new Error("localAppsProxy.sourceModel is unset");
  }
  const visibleCount = await evalOn(app, proxyId, "visibleCount");
  if (typeof visibleCount !== "number" || visibleCount < 0) {
    throw new Error(`visibleCount=${visibleCount} (expected non-negative number)`);
  }
  const outerRowCount = await evalOn(app, proxyId, "sourceModel.rowCount()");
  if (visibleCount > outerRowCount) {
    throw new Error(
      `visibleCount=${visibleCount} exceeds outer proxy rowCount=${outerRowCount} — ` +
      `matchLocalOnly cannot legally count MORE rows than its source`);
  }
});

test("app manager: 'local' header renders iff local rows exist", async (app) => {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Apps", "Categories"]); },
    { timeout: 15000, interval: 500, description: "App Manager to render" }
  );

  const proxyId = await findLocalAppsProxy(app);
  if (proxyId === null) throw new Error("appManager.localAppsProxy not found");

  const vcRes = await app.inspector.send("evaluate", {
    objectId: proxyId, expression: "visibleCount",
  });
  if (vcRes.error) throw new Error(`evaluate(visibleCount) failed: ${vcRes.error}`);
  const localCount = typeof vcRes.result === "number" ? vcRes.result : 0;

  // Section header label is lowercase "local" — distinguishes the
  // synthetic bucket from publisher-authored repo display names.
  const headerHits = await app.inspector.send("findByProperty", {
    property: "text", value: "local",
  });
  const visibleLocalHeaders = [];
  for (const m of (headerHits.matches ?? [])) {
    try {
      const props = await app.inspector.send("getProperties", { objectId: m.id });
      const visibleProp = props.properties?.find(p => p.name === "visible");
      if (visibleProp && visibleProp.value === true) visibleLocalHeaders.push(m.id);
    } catch { /* ignore per-match failures */ }
  }

  if (localCount > 0 && visibleLocalHeaders.length === 0) {
    throw new Error(
      `localAppsProxy.visibleCount=${localCount} but no visible "local" ` +
      `header was found in the AppManager tree`);
  }
  // Absence is fine when localCount === 0 — the section's `visible` binds
  // on localFilter.visibleCount > 0 and correctly hides.
});

// --- Tray Show/Hide (issue #268) ---
//
// Drives the real Window through the inspector: showHideWindow is a private
// slot, so QMetaObject::invokeMethod reaches it without needing a system tray
// (headless CI has no tray daemon). The unit tests cover Qt's window-state
// semantics; these cover our state machine on top of them.

async function windowObject(app) {
  const res = await app.inspector.send("findByProperty", {
    property: "objectName", value: "logosMainWindow",
  });
  const win = (res.matches ?? [])[0];
  if (!win) throw new Error("objectName=logosMainWindow not found");
  return win.id;
}

async function windowProps(app, objectId) {
  const res = await app.inspector.send("getProperties", { objectId });
  const read = (name) => {
    const value = res.properties?.find(p => p.name === name)?.value;
    if (typeof value !== "boolean") {
      throw new Error(
        `property "${name}" missing or not a boolean (got ${JSON.stringify(value)}) ` +
        `— the inspector contract changed and these assertions no longer guard anything`);
    }
    return value;
  };
  return { visible: read("visible"), minimized: read("minimized") };
}

async function invoke(app, objectId, method, what = `callMethod(${method})`) {
  const res = await app.inspector.send("callMethod", { objectId, method });
  if (res.error) throw new Error(`${what} failed: ${res.error}`);
}

test("window: tray toggle restores a minimized window on the first click", async (app) => {
  const win = await windowObject(app);
  try {
    await invoke(app, win, "showMinimized");
    await invoke(app, win, "showHideWindow");

    const { visible, minimized } = await windowProps(app, win);
    // Regression guard for #268: a minimized window is still visible() to Qt,
    // so a visibility-only toggle hid it again and the click did nothing.
    if (visible !== true || minimized === true) {
      throw new Error(
        `after one toggle: visible=${visible} minimized=${minimized} ` +
        `(expected visible=true minimized=false)`);
    }
  } finally {
    await invoke(app, win, "show");
  }
});

test("window: tray toggle hides a shown window", async (app) => {
  const win = await windowObject(app);
  try {
    await invoke(app, win, "show");
    await invoke(app, win, "showHideWindow");

    // Offscreen CI reports the window as active, so this can't cover the
    // background-window case: gating Hide on activation makes it unreachable
    // from the tray menu, and that stays a manual check.
    const { visible } = await windowProps(app, win);
    if (visible !== false) {
      throw new Error(`toggle left visible=${visible} (expected false)`);
    }
  } finally {
    await invoke(app, win, "show");
  }
});

// --- App-to-app intents -----------------------------------------------------
//
// These need the fixtures in tests/fixtures/intents staged into the app's
// --user-dir (see stage.sh). They are skipped when the fixtures are absent so a
// plain `node tests/ui-tests.mjs` against a normal install still runs green.

// ALWAYS resolve an objectId from the requester's own view before evaluating.
// The inspector falls back to the FIRST QQuickWidget's root when no objectId is
// given, and with several apps loaded that is very likely the wrong app — the
// assertion would then read a property that does not exist and pass vacuously.
//
// THROWS rather than returning null when the fixture is missing. An earlier
// version returned null and every caller did `if (!anchor) return;`, which meant
// that staging the fixtures but never LAUNCHING them produced three green ticks
// that had asserted nothing. A fixture that is not there is a broken test run,
// not a reason to skip.
async function requesterAnchor(app) {
  const found = await app.inspector.send("findByProperty", {
    property: "objectName", value: "requesterRoot",
  });
  if (!found.matches || !found.matches.length) {
    throw new Error(
      "intent fixture not loaded — expected an item with objectName " +
      "'requesterRoot'. Stage tests/fixtures/intents via stage.sh into the " +
      "--user-dir, and make sure the test opened the app first.");
  }
  return found.matches[0].id;
}

// The fixtures are staged on disk but not loaded until something opens them.
//
// Deliberately NOT openPlugin(): its expectTexts gate searches the shell's own
// QML tree and does not traverse into a plugin's separate engine, so it times
// out even after the app has loaded successfully. findByProperty does cross
// that boundary, so wait on the anchor itself.
async function openIntentRequester(app) {
  await app.click("Intent Requester");
  let anchor = null;
  await app.waitFor(async () => {
    const found = await app.inspector.send("findByProperty", {
      property: "objectName", value: "requesterRoot",
    });
    if (!found.matches || !found.matches.length)
      throw new Error("requester view not up yet");
    anchor = found.matches[0].id;
  }, { timeout: 20000, interval: 500, description: "intent requester view" });
  return anchor;
}

// Returns null when that provider has no view up at all, and its lastHandled
// (possibly "") when it does. The distinction matters: "not loaded" and "loaded
// but silent" are different failures, and the old version conflated them by
// returning the first match's value regardless of which provider it belonged to.
async function providerMarker(app, which) {
  // Each provider root carries a UNIQUE objectName (providerRootA / …B) so
  // both harnesses can address one directly. Returns null when that provider
  // has no view up at all, and its lastHandled (possibly "") when it does —
  // "not loaded" and "loaded but silent" are different failures.
  const suffix = which.replace("intent_provider_", "").toUpperCase();
  const found = await app.inspector.send("findByProperty", {
    property: "objectName", value: `providerRoot${suffix}`,
  });
  if (!found.matches || !found.matches.length) return null;
  const r = await app.inspector.send("evaluate", {
    objectId: found.matches[0].id, expression: "root.lastHandled",
  });
  return typeof r.result === "string" ? r.result : "";
}

// backend.currentVisibleApp — "which app is the user actually looking at".
//
// Read through the overlay root because it lives in the SHELL's engine, where
// `backend` is a context property; a fixture's anchor is in the plugin's own
// engine and has no `backend` at all. This is the observable auto-return moves,
// so every assertion below turns on it.
async function currentVisibleApp(app) {
  const overlay = await app.inspector.send("findByProperty", {
    property: "objectName", value: "overlayDialogs",
  });
  if (!overlay.matches || !overlay.matches.length)
    throw new Error("shell overlay not found — cannot read backend state");
  const r = await app.inspector.send("evaluate", {
    objectId: overlay.matches[0].id, expression: "backend.currentVisibleApp",
  });
  return typeof r.result === "string" ? r.result : "";
}

// Ask for `intent`, then answer the confirmation with intent_provider_manual.
// Returns once the provider's view is up and the shell has actually moved
// there — the precondition every auto-return assertion needs, and the one that
// makes "it never returned" distinguishable from "it never left".
async function dispatchToManualProvider(app, anchor, intent) {
  await app.inspector.send("evaluate", {
    objectId: anchor, expression: `root.request("${intent}")`,
  });

  let delegateId = null;
  await app.waitFor(async () => {
    const d = await app.inspector.send("findByProperty", {
      property: "objectName", value: "intentProvider_intent_provider_manual",
    });
    if (!d.matches || !d.matches.length)
      throw new Error("chooser has no delegate for intent_provider_manual");
    delegateId = d.matches[0].id;
  }, { timeout: 8000, interval: 250, description: "confirmation dialog" });

  await app.inspector.send("click", { objectId: delegateId });

  await app.waitFor(async () => {
    const visible = await currentVisibleApp(app);
    if (visible !== "intent_provider_manual")
      throw new Error(`shell is on "${visible}", not the provider`);
  }, { timeout: 45000, interval: 500, description: "dispatch moved the user" });
}

// Wait until the manual provider is actually HOLDING a request.
//
// dispatchTo() presents the provider BEFORE delivering to it, so the shell
// arriving is not evidence the QML handler has run. The fixture's buttons are
// disabled until it has, so clicking on the strength of the navigation alone
// silently does nothing and the test fails much later, somewhere else.
async function waitForManualProviderHolding(app) {
  await app.waitFor(async () => {
    const found = await app.inspector.send("findByProperty", {
      property: "objectName", value: "providerRootManual",
    });
    if (!found.matches || !found.matches.length)
      throw new Error("manual provider view not up");
    const handled = await app.inspector.send("evaluate", {
      objectId: found.matches[0].id, expression: "root.lastHandled",
    });
    if (handled.result !== "waiting")
      throw new Error(`provider is "${handled.result}", not holding a request`);
  }, { timeout: 20000, interval: 500, description: "provider holding the request" });
}

// Click a button inside the manual provider's view.
async function clickInManualProvider(app, objectName) {
  const found = await app.inspector.send("findByProperty", {
    property: "objectName", value: objectName,
  });
  if (!found.matches || !found.matches.length)
    throw new Error(`manual provider has no ${objectName}`);
  await app.inspector.send("click", { objectId: found.matches[0].id });
}

async function lastResult(app, anchorId) {
  return (await app.inspector.send("evaluate", {
    objectId: anchorId, expression: "root.lastResult",
  })).result;
}

test("intents: an undeclared intent is refused without asking anyone", async (app) => {
  const anchor = await openIntentRequester(app);

  await app.inspector.send("evaluate", {
    objectId: anchor, expression: 'root.request("test.undeclared")',
  });
  await app.waitFor(async () => {
    const r = await lastResult(app, anchor);
    if (r !== "not_declared") throw new Error(`got "${r}", expected not_declared`);
  }, { timeout: 5000, interval: 200, description: "not_declared" });
});

test("intents: two providers raise the chooser, and only the chosen one hears", async (app) => {
  const anchor = await openIntentRequester(app);

  await app.inspector.send("evaluate", {
    objectId: anchor, expression: 'root.request("test.echo")',
  });

  // The chooser must appear — with no chooser mounted the broker fails closed,
  // so this also covers the guard that used to be defeated by the shell's
  // signal re-emit.
  await app.waitFor(async () => {
    const d = await app.inspector.send("findByProperty", {
      property: "objectName", value: "intentChooserDialog",
    });
    if (!d.matches || !d.matches.length) throw new Error("chooser did not appear");
  }, { timeout: 8000, interval: 250, description: "intent chooser" });

  // Click the delegate BY OBJECT ID, never by text.
  //
  // app.click() is a breadth-first substring walk that stops at the first
  // clickable match, and "Provider B" also labels the app's SIDEBAR launcher.
  // Clicking that launches the app directly and leaves the request unresolved —
  // which looked exactly like a broken dispatch, and is the same trap this file
  // already documents for "Package Manager".
  const delegate = await app.inspector.send("findByProperty", {
    property: "objectName", value: "intentProvider_intent_provider_b",
  });
  if (!delegate.matches || !delegate.matches.length)
    throw new Error("chooser has no delegate for intent_provider_b");
  await app.inspector.send("click", { objectId: delegate.matches[0].id });

  // POSITIVE CONTROL AND ISOLATION IN ONE BODY. Asserting only that B stayed
  // empty passes trivially when nothing works at all — the requester's success
  // is what proves the path ran.
  //
  // The failure message names the STAGE the flow stalled at, because "got \"\""
  // is indistinguishable between "B never loaded", "B loaded but was never
  // dispatched to", and "B answered but the reply never arrived" — three very
  // different bugs. Offscreen runs are also slower than a real GUI, where this
  // path is known to work, so the budget is generous.
  await app.waitFor(async () => {
    const r = await lastResult(app, anchor);
    if (r === "ok:intent_provider_b") return;

    const bHandled = await providerMarker(app, "intent_provider_b");
    if (bHandled === null)
      throw new Error("provider_b view not up yet (still loading?)");
    if (!bHandled)
      throw new Error("provider_b loaded but never received the request");
    throw new Error(`provider_b is "${bHandled}" but requester still has "${r}"`);
  }, { timeout: 45000, interval: 500, description: "provider_b answered" });

  const aMarker = await providerMarker(app, "intent_provider_a");
  if (aMarker) throw new Error(`provider_a saw a request meant for b: "${aMarker}"`);
});

test("intents: a single provider still asks before dispatching", async (app) => {
  const anchor = await openIntentRequester(app);

  // test.solo is provided by intent_provider_a alone. One provider used to
  // dispatch straight through, which made it the SILENT case — an app that was
  // the only declarer of a capability got the request with no interaction at
  // all. Now it confirms like any other.
  await app.inspector.send("evaluate", {
    objectId: anchor, expression: 'root.request("test.solo")',
  });

  await app.waitFor(async () => {
    const d = await app.inspector.send("findByProperty", {
      property: "objectName", value: "intentChooserDialog",
    });
    if (!d.matches || !d.matches.length)
      throw new Error("single provider dispatched without asking");
  }, { timeout: 8000, interval: 250, description: "confirmation for one provider" });

  const delegate = await app.inspector.send("findByProperty", {
    property: "objectName", value: "intentProvider_intent_provider_a",
  });
  if (!delegate.matches || !delegate.matches.length)
    throw new Error("the sole provider is not offered in the dialog");
  await app.inspector.send("click", { objectId: delegate.matches[0].id });

  await app.waitFor(async () => {
    const r = await lastResult(app, anchor);
    if (r !== "ok:intent_provider_a") throw new Error(`got "${r}"`);
  }, { timeout: 45000, interval: 500, description: "provider_a answered" });
});

test("intents: answering a request returns the user to the caller", async (app) => {
  // The round trip. Dispatch already moves the user to the provider; this is
  // about the move BACK, which nothing did before — you approved something in
  // another app and were left standing there.
  //
  // The provider answers only when a button is pressed, so the return is
  // triggered by a real user action rather than by a timer, which is the whole
  // reason this fixture exists.
  const anchor = await openIntentRequester(app);
  await dispatchToManualProvider(app, anchor, "test.manual");

  await waitForManualProviderHolding(app);
  await clickInManualProvider(app, "btnComplete");

  await app.waitFor(async () => {
    const visible = await currentVisibleApp(app);
    if (visible !== "intent_requester_demo")
      throw new Error(`still on "${visible}" — the user was never brought back`);
  }, { timeout: 20000, interval: 250, description: "returned to the requester" });

  // Navigation only. The result must still have been delivered — a return that
  // swallowed the answer would be worse than no return.
  const r = await lastResult(app, anchor);
  if (r !== "ok:intent_provider_manual")
    throw new Error(`returned, but the requester got "${r}"`);
});

test("intents: cancelling also returns the user to the caller", async (app) => {
  // Backing out is the outcome that most wants a ride home — the user decided
  // not to do the thing, and being parked in the provider afterwards is the
  // worst of both.
  const anchor = await openIntentRequester(app);
  await dispatchToManualProvider(app, anchor, "test.manual");

  await waitForManualProviderHolding(app);
  await clickInManualProvider(app, "btnCancel");

  await app.waitFor(async () => {
    const visible = await currentVisibleApp(app);
    if (visible !== "intent_requester_demo")
      throw new Error(`still on "${visible}" after a cancel`);
  }, { timeout: 20000, interval: 250, description: "returned after cancel" });

  const r = await lastResult(app, anchor);
  if (r !== "cancelled") throw new Error(`expected cancelled, got "${r}"`);
});

test("intents: a hand-off leaves the user where it took them", async (app) => {
  // THE PAIR IS THE POINT. Identical provider, identical button, identical
  // ok:true — differing only by "handoff": true in metadata.json. If the shell
  // returns here, the declaration is not being read.
  const anchor = await openIntentRequester(app);
  await dispatchToManualProvider(app, anchor, "test.handoff");

  // Same button, same moment in the flow as the transaction above. WHEN a
  // provider answers is its own business; `handoff` governs only what the
  // shell does next, and holding both constant is what isolates that.
  await waitForManualProviderHolding(app);
  await clickInManualProvider(app, "btnComplete");

  await app.waitFor(async () => {
    const r = await lastResult(app, anchor);
    if (r !== "ok:intent_provider_manual")
      throw new Error(`hand-off not answered yet, requester has "${r}"`);
  }, { timeout: 20000, interval: 250, description: "hand-off answered" });

  // The answer has landed. Give the dwell floor room to fire a return if the
  // guard is broken — asserting immediately would pass even with the feature
  // misbehaving, because the wrong behaviour is merely late, not absent.
  await new Promise((resolve) => setTimeout(resolve, 1500));

  const visible = await currentVisibleApp(app);
  if (visible !== "intent_provider_manual")
    throw new Error(
      `a hand-off bounced the user to "${visible}" — the whole point is that ` +
      "they were sent somewhere to stay");
});

test("intents: a payload the provider declared unusable never reaches it", async (app) => {
  const anchor = await openIntentRequester(app);

  // intent_provider_a's metadata.json says test.solo takes `text` as a string.
  // This sends a number. Nothing about it is malformed as data — only the
  // provider's own declaration makes it wrong, which is the whole point of
  // declaring params at all.
  await app.inspector.send("evaluate", {
    objectId: anchor, expression: 'root.requestBadParams("test.solo")',
  });

  // The user is still asked. Validation happens after a provider is settled,
  // never at submit: at submit several providers may describe one intent
  // differently, and testing all their specs would answer "how many providers
  // are there".
  await app.waitFor(async () => {
    const d = await app.inspector.send("findByProperty", {
      property: "objectName", value: "intentChooserDialog",
    });
    if (!d.matches || !d.matches.length) throw new Error("no confirmation shown");
  }, { timeout: 8000, interval: 250, description: "confirmation for one provider" });

  const delegate = await app.inspector.send("findByProperty", {
    property: "objectName", value: "intentProvider_intent_provider_a",
  });
  if (!delegate.matches || !delegate.matches.length)
    throw new Error("the sole provider is not offered in the dialog");
  await app.inspector.send("click", { objectId: delegate.matches[0].id });

  await app.waitFor(async () => {
    const r = await lastResult(app, anchor);
    if (r !== "bad_request") throw new Error(`got "${r}"`);
  }, { timeout: 20000, interval: 500, description: "bad_request reached the caller" });

  // And the provider never saw it. A payload it declared unusable must not
  // reach its handler — otherwise the declaration is documentation, not a gate.
  const provider = await app.inspector.send("findByProperty", {
    property: "objectName", value: "providerRootA",
  });
  if (provider.matches && provider.matches.length) {
    const handled = await app.inspector.send("evaluate", {
      objectId: provider.matches[0].id, expression: "root.pendingParams",
    });
    const seen = JSON.stringify(handled && handled.result);
    if (seen && seen.includes("42"))
      throw new Error("the provider received a payload it declared unusable");
  }
});

test("intents: every permitted data shape survives the round trip", async (app) => {
  const anchor = await openIntentRequester(app);

  // The transport, not the mechanism. `params` crosses from the requester's QML
  // engine into C++, through the broker, into the PROVIDER's separate engine —
  // and the reply makes the same trip back through `respond`'s untyped
  // QVariant, which is where engine-bound values were previously lost silently
  // (res.data arrived null with no error anywhere).
  //
  // The fixture compares structurally and reports the first differing path, so
  // a failure names the field rather than just saying "not equal".
  await app.inspector.send("evaluate", {
    objectId: anchor, expression: "root.requestRoundTrip()",
  });

  // test.roundtrip has one provider, and one provider still confirms.
  await app.waitFor(async () => {
    const d = await app.inspector.send("findByProperty", {
      property: "objectName", value: "intentChooserDialog",
    });
    if (!d.matches || !d.matches.length) throw new Error("no confirmation shown");
  }, { timeout: 8000, interval: 250, description: "chooser for test.roundtrip" });

  const delegate = await app.inspector.send("findByProperty", {
    property: "objectName", value: "intentProvider_intent_provider_a",
  });
  if (!delegate.matches || !delegate.matches.length)
    throw new Error("provider_a not offered");
  await app.inspector.send("click", { objectId: delegate.matches[0].id });

  await app.waitFor(async () => {
    const r = await lastResult(app, anchor);
    if (r === "") throw new Error("still waiting");
    if (r !== "roundtrip:ok") throw new Error(r);   // carries the differing path
  }, { timeout: 30000, interval: 500, description: "payload returned intact" });
});

test("intents: the word ambiguous never reaches a requester", async (app) => {
  const anchor = await openIntentRequester(app);
  const r = await lastResult(app, anchor);
  if (typeof r === "string" && r.includes("ambiguous")) {
    throw new Error("internal resolution state leaked into an envelope");
  }
});

// --- Run ---

run();
