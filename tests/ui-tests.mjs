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
import { writeSync } from "node:fs";
import { findByObjectName, makeTest, sleep } from "./fixtures/harness.mjs";
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

// --- Welcome page (A1) — must run FIRST: asserts the pre-interaction state ---
async function findWelcomePage(app) {
  const res = typeof app.findByType === "function"
    ? await app.findByType("WelcomePage")
    : await app.inspector.send("findByType", { typeName: "WelcomePage" });
  if (res.error) throw new Error(`findByType(WelcomePage) failed: ${res.error}`);
  return (res.matches ?? [])[0] || null;
}

test("welcome: first launch shows the welcome page", async (app) => {
  let welcome = null;
  await app.waitFor(async () => {
    welcome = await findWelcomePage(app);
    if (!welcome) throw new Error("no WelcomePage instance in the QML tree");
  }, { timeout: 10000, interval: 500, description: "WelcomePage instance to exist" });

  const visRes = await app.inspector.send("evaluate", {
    objectId: welcome.id, expression: "visible",
  });
  if (visRes.error) throw new Error(`evaluate(visible) failed: ${visRes.error}`);
  if (visRes.result !== true) {
    throw new Error(`WelcomePage visible=${visRes.result} (expected true)`);
  }

  // launcherApps populates asynchronously — check length + greeting in one retried step.
  await app.waitFor(async () => {
    const lenRes = await app.inspector.send("evaluate", {
      objectId: welcome.id, expression: "backend.launcherApps.length",
    });
    if (lenRes.error) {
      throw new Error(`evaluate(backend.launcherApps.length) failed: ${lenRes.error}`);
    }
    if (typeof lenRes.result !== "number") {
      throw new Error(
        `backend.launcherApps.length=${JSON.stringify(lenRes.result)} (expected number)`);
    }
    const expected = lenRes.result === 0 ? "Welcome to Basecamp!" : "Welcome back";
    await app.expectTexts([expected]);
  }, { timeout: 10000, interval: 500, description: "greeting to match backend.launcherApps" });

  const greetingRes = await app.inspector.send("evaluate", {
    objectId: welcome.id,
    expression: `(() => {
      const hasText = (node, expected) => {
        if (!node) return false;
        if (typeof node.text === "string" && node.text.includes(expected)) return true;
        if (!node.children || typeof node.children.length !== "number") return false;
        for (let i = 0; i < node.children.length; i += 1) {
          if (hasText(node.children[i], expected)) return true;
        }
        return false;
      };
      // The inspector serializes object results as "<QJSValue>" — return JSON.
      return JSON.stringify({
        hasFirstLaunch: hasText(this, "Welcome to Basecamp!"),
        hasWelcomeBack: hasText(this, "Welcome back"),
      });
    })()`,
  });
  if (greetingRes.error) {
    throw new Error(`evaluate(greeting presence) failed: ${greetingRes.error}`);
  }
  const greeting = JSON.parse(greetingRes.result);
  const hasFirstLaunch = greeting?.hasFirstLaunch === true;
  const hasWelcomeBack = greeting?.hasWelcomeBack === true;
  if (hasFirstLaunch === hasWelcomeBack) {
    throw new Error(
      `greeting texts present: "Welcome to Basecamp!"=${hasFirstLaunch}, ` +
      `"Welcome back"=${hasWelcomeBack} (expected exactly one)`);
  }
});

const CI_MODE = process.argv.includes("--ci");
// --- Welcome page (A2) — must run right after A1: navigating clicks the
// welcome page away ---
test('welcome: "Install now" navigates to Applications', async (app) => {
  // Prefer the spec objectName; fall back to the button text until
  // "welcomePage.installNow" exists in WelcomePage.qml.
  let button = null;
  await app.waitFor(async () => {
    const byName = await app.findByProperty("objectName", "welcomePage.installNow");
    button = (byName.matches ?? [])[0] || null;
    if (!button) {
      const byText = await app.findByProperty("text", "Install now");
      button = (byText.matches ?? []).find((m) => (m.type ?? "").includes("Button")) || null;
    }
    if (!button) throw new Error('"Install now" button not found on the welcome page');
  }, { timeout: 10000, interval: 500, description: '"Install now" button to exist' });

  // Signal-level click — coordinate hit-testing on offscreen is fragile
  // (see installViaPmu); the onClicked handler chain is identical.
  const clicked = await app.inspector.send("callMethod", {
    objectId: button.id, method: "clicked",
  });
  if (clicked.error) throw new Error(`callMethod(clicked) failed: ${clicked.error}`);

  await app.waitFor(
    async () => { await app.expectTexts(["Install and manage applications."]); },
    { timeout: 10000, interval: 500, description: "Applications view to render" }
  );

  // The sidebar "Applications" button carries the section index it activates
  // (onClicked passes _d.workspaceSections.length + index) — read it from the
  // delegate's context instead of hard-coding the sidebar layout. Only
  // objects in SidebarPanel's delegate scope can resolve the expression, so
  // it also disambiguates the button from same-text headers.
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
    const res = await app.inspector.send("evaluate", {
      objectId: appsButtonId, expression: "backend.currentActiveSectionIndex",
    });
    if (res.error) {
      throw new Error(`evaluate(backend.currentActiveSectionIndex) failed: ${res.error}`);
    }
    if (res.result !== applicationsIndex) {
      throw new Error(
        `backend.currentActiveSectionIndex=${res.result} ` +
        `(expected Applications index ${applicationsIndex})`);
    }
  }, { timeout: 10000, interval: 500, description: "active section to become Applications" });

  // WelcomePage's own QML `visible` stays true inside its offscreen
  // QQuickWidget host; what observably hides it is that host — WorkspaceArea
  // (objectName "workspace"), the stack page the section switch left.
  const wsHits = await app.findByProperty("objectName", "workspace");
  const workspace = (wsHits.matches ?? [])[0];
  if (!workspace) throw new Error("workspace area (welcome page host) not found");
  const props = await app.inspector.send("getProperties", { objectId: workspace.id });
  const visible = props.properties?.find((p) => p.name === "visible")?.value;
  if (visible !== false) {
    throw new Error(
      `welcome page still visible: workspace visible=` +
      `${JSON.stringify(visible)} (expected false)`);
  }
});

// --- Workspace (A3) — opening an app replaces the welcome page with a dock ---
// Runs after A2: it hides the welcome host and leaves a dock open, so it must
// not sit between A1 and A2 (A2 asserts the pre-navigation welcome state).
//
// Fixture A (test_qml_only, spec §0.A) is pre-seeded into <user-dir>/plugins/
// by nix/integration-test.nix, so in --ci mode its sidebar tile is guaranteed
// to appear once launcherApps populates. When attached to a locally running
// app without the fixture, spec §0.A says skip, not fail — --ci in argv is
// how the two cases are told apart (see the integration-test invocation).
//
// The dock check uses WorkspaceArea's dockCount test hook rather than the
// spec's workspace.dock.<name> objectName: DockCard's objectName is the
// constant "dockCard" on this branch (src/WorkspaceArea.cpp:96).

// Payload text rendered by fixture A's Main.qml (see qmlViewFor in
// tests/fixtures/lgx.mjs) — derived from FIXTURE_A so it can't drift.
const FIXTURE_A_TEXT =
  `${FIXTURE_A.displayName} (${FIXTURE_A.name}) v${FIXTURE_A.version}`;

// Welcome visibility lives on the hosting QQuickWidget —
// WorkspaceArea::updateWelcomeVisibility() hides the widget, not the QML
// item (the C++ unit tests assert isVisibleTo on the widget for the same
// reason) — and that widget has no objectName. Locate it by source URL
// among QQuickWidget instances; fall back to the item's Window attached
// property (QQuickWidget mirrors widget show/hide onto its offscreen
// window).
async function welcomePageHidden(app, welcomeItemId) {
  const byType = await app.inspector.send("findByType", { typeName: "QQuickWidget" });
  for (const m of byType.matches ?? []) {
    const props = await app.inspector.send("getProperties", { objectId: m.id });
    const source = props.properties?.find((p) => p.name === "source")?.value;
    if (typeof source === "string" && source.includes("WelcomePage.qml")) {
      const visible = props.properties?.find((p) => p.name === "visible")?.value;
      if (typeof visible === "boolean") return !visible;
    }
  }
  const winRes = await app.inspector.send("evaluate", {
    objectId: welcomeItemId, expression: "Window.visible",
  });
  if (typeof winRes.result === "boolean") return !winRes.result;
  throw new Error(
    "cannot determine welcome-page visibility: no QQuickWidget with a " +
    "WelcomePage.qml source found, and Window.visible did not evaluate " +
    "to a boolean");
}

test("workspace: opening an app replaces the welcome page with a dock", async (app) => {
  // Stable evaluate anchor with `backend` in context. The sidebar tile
  // cannot anchor the post-click waits: launching moves the app from the
  // unloaded to the loaded Repeater, destroying the clicked delegate.
  let welcome = null;
  await app.waitFor(async () => {
    welcome = await findWelcomePage(app);
    if (!welcome) throw new Error("no WelcomePage instance in the QML tree");
  }, { timeout: 10000, interval: 500, description: "WelcomePage instance to exist" });

  // App tiles render icon-only (name is a tooltip), so click by the
  // §4.1 automation objectName, not by text.
  let tile = null;
  try {
    await app.waitFor(async () => {
      tile = await findByObjectName(app.inspector, `sidebar.app.${FIXTURE_A.name}`);
      if (!tile) throw new Error(`sidebar.app.${FIXTURE_A.name} not in the tree`);
    }, { timeout: 10000, interval: 500, description: "fixture A sidebar tile to appear" });
  } catch (e) {
    if (!CI_MODE) {
      console.log(
        `    SKIP: fixture A (${FIXTURE_A.name}) is not installed in this ` +
        `app instance (spec §0.A: skip, not fail, outside --ci)`);
      return;
    }
    throw new Error(
      `fixture A sidebar tile never appeared — integration-test pre-seeds ` +
      `${FIXTURE_A.name} at boot, so this is a real failure: ${e.message}`);
  }

  const workspace = await findByObjectName(app.inspector, "workspace");
  if (!workspace) {
    throw new Error('WorkspaceArea (objectName "workspace") not found');
  }

  const clicked = await app.inspector.send("callMethod", {
    objectId: tile.id, method: "clicked",
  });
  if (clicked.error) {
    throw new Error(`clicking sidebar.app.${FIXTURE_A.name} failed: ${clicked.error}`);
  }

  // Gate: the backend reports the app front-most within 10 s.
  await app.waitFor(async () => {
    const res = await app.inspector.send("evaluate", {
      objectId: welcome.id, expression: "backend.currentVisibleApp",
    });
    if (res.error) {
      throw new Error(`evaluate(backend.currentVisibleApp) failed: ${res.error}`);
    }
    if (res.result !== FIXTURE_A.name) {
      throw new Error(
        `backend.currentVisibleApp=${JSON.stringify(res.result)} ` +
        `(expected "${FIXTURE_A.name}")`);
    }
  }, { timeout: 10000, interval: 500,
       description: `currentVisibleApp to become "${FIXTURE_A.name}"` });

  // A dock for it exists.
  await app.waitFor(async () => {
    const res = await app.inspector.send("evaluate", {
      objectId: workspace.id, expression: "dockCount",
    });
    if (res.error) throw new Error(`evaluate(dockCount) failed: ${res.error}`);
    if (res.result !== 1) {
      throw new Error(`WorkspaceArea.dockCount=${res.result} (expected 1)`);
    }
  }, { timeout: 10000, interval: 500, description: "workspace dockCount to reach 1" });

  // The welcome page is no longer visible…
  await app.waitFor(async () => {
    if ((await welcomePageHidden(app, welcome.id)) !== true) {
      throw new Error("welcome page is still visible after the dock opened");
    }
  }, { timeout: 5000, interval: 250, description: "welcome page to hide" });

  // …and fixture A's payload text renders — the app actually loaded.
  await app.waitFor(
    async () => { await app.expectTexts([FIXTURE_A_TEXT]); },
    { timeout: 10000, interval: 500, description: "fixture A payload text to render" }
  );

  // Leave the dock open: the A4 follow-up owns close-the-dock coverage
  // (workspace.closeDock), and no later test asserts welcome-page state.
});
// Click options that pin a click to a sidebar SECTION button and nothing else.
//
// qt-mcp's findAndClick is a breadth-first walk that SUBSTRING-matches the
// `text` property and stops at the first object cmdClick accepts — and
// cmdClick on a QWidget can never fail, it just posts a mouse event at the
// widget's centre and reports success. MainContainer's PMUI placeholder is a
// QLabel reading "Loading Package Manager…", which contains "Package Manager"
// and sits SHALLOWER in that walk than the sidebar's QML button
// (MainContainer > contentArea > QStackedWidget > placeholder > QLabel, vs
// the sidebar's Control > contentItem > ColumnLayout > delegate).
//
// So `app.click("Package Manager")` clicked the placeholder label, reported
// success, and left the section index untouched. Measured, before this fix:
//
//   clicked -> {"matchedText":"Loading Package Manager…","matchedType":"QLabel"}
//   MainContainer: Active section index changed to 1 / 3   (never 2)
//   ...no "Loading UI module: package_manager_ui", no ui-host, ever
//
// With { exact: true, type: "SidebarCircleButton" } it resolves to the real
// button and the section actually opens:
//
//   clicked -> {"matchedText":"Package Manager","matchedType":"SidebarCircleButton_QMLTYPE_<n>"}
//   MainContainer: Active section index changed to 2
//   Loading UI module: "package_manager_ui" -> ViewModuleHost: spawning ui-host
//   -> Successfully loaded UI module: "package_manager_ui"
const sidebarSection = { exact: true, type: "SidebarCircleButton" };

// --- Workspace (A4) — closing the last dock brings the welcome page back ---
//
// Spec §2.A A4: from A3 state, close fixture A's dock. Closing the last dock
// also unloads the module by design (WorkspaceArea::pluginClosed →
// unloadUiModule), so the gates double as a regression guard for the
// currentVisibleApp clear on unload of the visible app.
//
// closeDock is invoked through the inspector's evaluate, NOT callMethod:
// callMethod does not marshal the QString argument correctly (logos-qt-mcp
// limitation), while the evaluate path's JS engine converts it fine.

test("workspace: closing the last dock brings the welcome page back", async (app) => {
  // Same stable evaluate anchor as A3 — has `backend` in context and
  // survives the dock teardown.
  let welcome = null;
  await app.waitFor(async () => {
    welcome = await findWelcomePage(app);
    if (!welcome) throw new Error("no WelcomePage instance in the QML tree");
  }, { timeout: 10000, interval: 500, description: "WelcomePage instance to exist" });

  const workspace = await findByObjectName(app.inspector, "workspace");
  if (!workspace) {
    throw new Error('WorkspaceArea (objectName "workspace") not found');
  }

  // Establish the A3 end state without assuming A3 left it: fixture A's
  // dock must be open before we can close it.
  const preCount = await app.inspector.send("evaluate", {
    objectId: workspace.id, expression: "dockCount",
  });
  if (preCount.error) throw new Error(`evaluate(dockCount) failed: ${preCount.error}`);
  if (preCount.result !== 1) {
    let tile = null;
    try {
      await app.waitFor(async () => {
        tile = await findByObjectName(app.inspector, `sidebar.app.${FIXTURE_A.name}`);
        if (!tile) throw new Error(`sidebar.app.${FIXTURE_A.name} not in the tree`);
      }, { timeout: 10000, interval: 500, description: "fixture A sidebar tile to appear" });
    } catch (e) {
      if (!CI_MODE) {
        console.log(
          `    SKIP: fixture A (${FIXTURE_A.name}) is not installed in this ` +
          `app instance (spec §0.A: skip, not fail, outside --ci)`);
        return;
      }
      throw new Error(
        `no dock open and fixture A sidebar tile never appeared — ` +
        `integration-test pre-seeds ${FIXTURE_A.name} at boot, so this is ` +
        `a real failure: ${e.message}`);
    }
    const clicked = await app.inspector.send("callMethod", {
      objectId: tile.id, method: "clicked",
    });
    if (clicked.error) {
      throw new Error(`clicking sidebar.app.${FIXTURE_A.name} failed: ${clicked.error}`);
    }
  }
  await app.waitFor(async () => {
    const count = await app.inspector.send("evaluate", {
      objectId: workspace.id, expression: "dockCount",
    });
    if (count.error) throw new Error(`evaluate(dockCount) failed: ${count.error}`);
    if (count.result !== 1) {
      throw new Error(`WorkspaceArea.dockCount=${count.result} (expected 1)`);
    }
    const visibleApp = await app.inspector.send("evaluate", {
      objectId: welcome.id, expression: "backend.currentVisibleApp",
    });
    if (visibleApp.error) {
      throw new Error(`evaluate(backend.currentVisibleApp) failed: ${visibleApp.error}`);
    }
    if (visibleApp.result !== FIXTURE_A.name) {
      throw new Error(
        `backend.currentVisibleApp=${JSON.stringify(visibleApp.result)} ` +
        `(expected "${FIXTURE_A.name}")`);
    }
  }, { timeout: 10000, interval: 500,
       description: `fixture A dock to be open and front-most` });

  // Close the dock.
  const closed = await app.inspector.send("evaluate", {
    objectId: workspace.id,
    expression: `closeDock(${JSON.stringify(FIXTURE_A.name)})`,
  });
  if (closed.error) throw new Error(`evaluate(closeDock) failed: ${closed.error}`);

  // Gate: dock count reaches 0 within 5 s.
  await app.waitFor(async () => {
    const res = await app.inspector.send("evaluate", {
      objectId: workspace.id, expression: "dockCount",
    });
    if (res.error) throw new Error(`evaluate(dockCount) failed: ${res.error}`);
    if (res.result !== 0) {
      throw new Error(`WorkspaceArea.dockCount=${res.result} (expected 0)`);
    }
  }, { timeout: 5000, interval: 250, description: "workspace dockCount to reach 0" });

  // Gate: the welcome page is visible again…
  await app.waitFor(async () => {
    if ((await welcomePageHidden(app, welcome.id)) !== false) {
      throw new Error("welcome page is still hidden after closing the last dock");
    }
  }, { timeout: 5000, interval: 250, description: "welcome page to reappear" });

  // …with the installed-apps greeting — closing unloads fixture A but does
  // not uninstall it, so launcherApps stays non-empty and the greeting is
  // "Welcome back", not the first-launch text.
  await app.waitFor(
    async () => { await app.expectTexts(["Welcome back"]); },
    { timeout: 5000, interval: 250, description: '"Welcome back" greeting to render' }
  );

  // Gate: the backend no longer reports a front-most app.
  await app.waitFor(async () => {
    const res = await app.inspector.send("evaluate", {
      objectId: welcome.id, expression: "backend.currentVisibleApp",
    });
    if (res.error) {
      throw new Error(`evaluate(backend.currentVisibleApp) failed: ${res.error}`);
    }
    if (res.result !== "") {
      throw new Error(
        `backend.currentVisibleApp=${JSON.stringify(res.result)} (expected "")`);
    }
  }, { timeout: 5000, interval: 250, description: "currentVisibleApp to clear" });
});

// --- Workspace (A5) — re-clicking an open app does not create a second dock ---
//
// Spec §2.A A5: click sidebar.app.test_qml_only twice, 500 ms apart. The
// first click opens the dock (A4 left the workspace empty); the second must
// activate the existing dock, not spawn another.
//
// "Exactly one instance of fixture A's root item type in the tree" cannot be
// checked literally on this branch: the fixture's root is a plain Rectangle
// (qmlViewFor in tests/fixtures/lgx.mjs), a type the shell instantiates all
// over. Each instantiation of the fixture's root document lives in exactly
// one host QQuickWidget whose source is <installDir>/Main.qml
// (PluginLoader.cpp:367) and renders exactly one Text with the unique
// payload string — so those two counts stand in for the root-type count.

test("workspace: re-clicking an open app does not create a second dock", async (app) => {
  // Same stable evaluate anchor as A3/A4 — has `backend` in context and
  // survives sidebar delegate churn.
  let welcome = null;
  await app.waitFor(async () => {
    welcome = await findWelcomePage(app);
    if (!welcome) throw new Error("no WelcomePage instance in the QML tree");
  }, { timeout: 10000, interval: 500, description: "WelcomePage instance to exist" });

  const workspace = await findByObjectName(app.inspector, "workspace");
  if (!workspace) {
    throw new Error('WorkspaceArea (objectName "workspace") not found');
  }

  // Click #1 — opens the dock.
  let tile = null;
  try {
    await app.waitFor(async () => {
      tile = await findByObjectName(app.inspector, `sidebar.app.${FIXTURE_A.name}`);
      if (!tile) throw new Error(`sidebar.app.${FIXTURE_A.name} not in the tree`);
    }, { timeout: 10000, interval: 500, description: "fixture A sidebar tile to appear" });
  } catch (e) {
    if (!CI_MODE) {
      console.log(
        `    SKIP: fixture A (${FIXTURE_A.name}) is not installed in this ` +
        `app instance (spec §0.A: skip, not fail, outside --ci)`);
      return;
    }
    throw new Error(
      `fixture A sidebar tile never appeared — integration-test pre-seeds ` +
      `${FIXTURE_A.name} at boot, so this is a real failure: ${e.message}`);
  }
  const firstClick = await app.inspector.send("callMethod", {
    objectId: tile.id, method: "clicked",
  });
  if (firstClick.error) {
    throw new Error(`clicking sidebar.app.${FIXTURE_A.name} failed: ${firstClick.error}`);
  }

  // Wait until the app is actually open — the spec's 500 ms spacing assumes
  // the first click's dock exists before the re-click; on a slow-loading run
  // a blind 500 ms click would test click-while-loading instead.
  await app.waitFor(async () => {
    const count = await app.inspector.send("evaluate", {
      objectId: workspace.id, expression: "dockCount",
    });
    if (count.error) throw new Error(`evaluate(dockCount) failed: ${count.error}`);
    if (count.result !== 1) {
      throw new Error(`WorkspaceArea.dockCount=${count.result} (expected 1)`);
    }
    const visibleApp = await app.inspector.send("evaluate", {
      objectId: welcome.id, expression: "backend.currentVisibleApp",
    });
    if (visibleApp.error) {
      throw new Error(`evaluate(backend.currentVisibleApp) failed: ${visibleApp.error}`);
    }
    if (visibleApp.result !== FIXTURE_A.name) {
      throw new Error(
        `backend.currentVisibleApp=${JSON.stringify(visibleApp.result)} ` +
        `(expected "${FIXTURE_A.name}")`);
    }
  }, { timeout: 10000, interval: 500,
       description: "fixture A dock to open after the first click" });

  // Click #2, 500 ms later. Loading moved the delegate from the unloaded to
  // the loaded Repeater (same objectName, new object), so re-find inside the
  // retry loop — a delegate mid-churn just retries, and a duplicate
  // activation click is harmless (activation is what A5 exercises).
  await sleep(500);
  await app.waitFor(async () => {
    const loadedTile =
      await findByObjectName(app.inspector, `sidebar.app.${FIXTURE_A.name}`);
    if (!loadedTile) throw new Error(`sidebar.app.${FIXTURE_A.name} not in the tree`);
    const clicked = await app.inspector.send("callMethod", {
      objectId: loadedTile.id, method: "clicked",
    });
    if (clicked.error) {
      throw new Error(`re-clicking sidebar.app.${FIXTURE_A.name} failed: ${clicked.error}`);
    }
  }, { timeout: 10000, interval: 500, description: "second click on fixture A tile" });

  // Gate: dock count STAYS 1 — poll across a settle window rather than one
  // instant-passing read, so an asynchronously created second dock (the
  // load path defers through singleShot timers) cannot slip in unseen.
  const settleDeadline = Date.now() + 2000;
  for (;;) {
    const count = await app.inspector.send("evaluate", {
      objectId: workspace.id, expression: "dockCount",
    });
    if (count.error) throw new Error(`evaluate(dockCount) failed: ${count.error}`);
    if (count.result !== 1) {
      throw new Error(
        `WorkspaceArea.dockCount=${count.result} after re-click ` +
        `(expected it to stay 1)`);
    }
    if (Date.now() >= settleDeadline) break;
    await sleep(250);
  }

  // Gate: fixture A is still the front-most app.
  const visibleApp = await app.inspector.send("evaluate", {
    objectId: welcome.id, expression: "backend.currentVisibleApp",
  });
  if (visibleApp.error) {
    throw new Error(`evaluate(backend.currentVisibleApp) failed: ${visibleApp.error}`);
  }
  if (visibleApp.result !== FIXTURE_A.name) {
    throw new Error(
      `backend.currentVisibleApp=${JSON.stringify(visibleApp.result)} ` +
      `(expected "${FIXTURE_A.name}")`);
  }

  // Gate: exactly one instantiation of fixture A's root document — one host
  // QQuickWidget sourced from the fixture's Main.qml…
  const byType = await app.inspector.send("findByType", { typeName: "QQuickWidget" });
  if (byType.error) throw new Error(`findByType(QQuickWidget) failed: ${byType.error}`);
  const fixtureHosts = [];
  for (const m of byType.matches ?? []) {
    const props = await app.inspector.send("getProperties", { objectId: m.id });
    const source = props.properties?.find((p) => p.name === "source")?.value;
    if (typeof source === "string"
        && source.includes(`/${FIXTURE_A.name}/`)
        && source.endsWith("Main.qml")) {
      fixtureHosts.push(source);
    }
  }
  if (fixtureHosts.length !== 1) {
    throw new Error(
      `${fixtureHosts.length} QQuickWidget(s) sourced from fixture A's ` +
      `Main.qml (expected exactly 1): ${JSON.stringify(fixtureHosts)}`);
  }

  // …and exactly one render of its unique payload text.
  const textHits = await app.inspector.send("findByProperty", {
    property: "text", value: FIXTURE_A_TEXT,
  });
  if (textHits.error) {
    throw new Error(`findByProperty(text=payload) failed: ${textHits.error}`);
  }
  const payloadCount = (textHits.matches ?? []).length;
  if (payloadCount !== 1) {
    throw new Error(
      `${payloadCount} instance(s) of fixture A's payload text in the tree ` +
      `(expected exactly 1)`);
  }

  // Cleanup: close the dock so the rest of the suite starts from the same
  // no-docks baseline A4 established (close also unloads the module —
  // same evaluate path as A4; callMethod can't marshal the QString arg).
  const closed = await app.inspector.send("evaluate", {
    objectId: workspace.id,
    expression: `closeDock(${JSON.stringify(FIXTURE_A.name)})`,
  });
  if (closed.error) throw new Error(`evaluate(closeDock) failed: ${closed.error}`);
  await app.waitFor(async () => {
    const res = await app.inspector.send("evaluate", {
      objectId: workspace.id, expression: "dockCount",
    });
    if (res.error) throw new Error(`evaluate(dockCount) failed: ${res.error}`);
    if (res.result !== 0) {
      throw new Error(`WorkspaceArea.dockCount=${res.result} (expected 0)`);
    }
  }, { timeout: 5000, interval: 250, description: "cleanup: fixture A dock to close" });
});

// --- Sidebar (A6) — footer shows the build type, with the version when present ---
//
// Spec §2.A A6 (amended 2026-08-28). Expectations are DERIVED from
// backend.buildVersion / backend.isPortableBuild, never hardcoded — nix
// builds bake "0.0.0-dev" when VERSION is absent (buildVersion is never
// empty there), but a non-nix build can legitimately have an empty
// buildVersion, in which case the footer is the build-type token alone.
//
// The assertion is scoped to the one footer element (SidebarPanel.qml's
// LogosSelectableText, objectName "sidebar.buildLabel"), not a page-wide
// text search: DashboardView renders its own "Dev build" string, so a
// tree-wide substring match could pass against the wrong element.
//
// Read-only against the sidebar — no clicks, no workspace/dock changes.

test("sidebar: footer shows the build type, with the version when present", async (app) => {
  let footer = null;
  await app.waitFor(async () => {
    footer = await findByObjectName(app.inspector, "sidebar.buildLabel");
    if (!footer) throw new Error("sidebar.buildLabel not in the QML tree");
  }, { timeout: 10000, interval: 500, description: "sidebar build-label footer to exist" });

  const versionRes = await app.inspector.send("evaluate", {
    objectId: footer.id, expression: "backend.buildVersion",
  });
  if (versionRes.error) {
    throw new Error(`evaluate(backend.buildVersion) failed: ${versionRes.error}`);
  }
  const buildVersion = versionRes.result;
  if (typeof buildVersion !== "string") {
    throw new Error(
      `backend.buildVersion=${JSON.stringify(buildVersion)} (expected string)`);
  }

  const portableRes = await app.inspector.send("evaluate", {
    objectId: footer.id, expression: "backend.isPortableBuild",
  });
  if (portableRes.error) {
    throw new Error(`evaluate(backend.isPortableBuild) failed: ${portableRes.error}`);
  }
  const isPortable = portableRes.result;
  if (typeof isPortable !== "boolean") {
    throw new Error(
      `backend.isPortableBuild=${JSON.stringify(isPortable)} (expected boolean)`);
  }

  const textRes = await app.inspector.send("evaluate", {
    objectId: footer.id, expression: "text",
  });
  if (textRes.error) throw new Error(`evaluate(text) failed: ${textRes.error}`);
  const text = textRes.result;
  if (typeof text !== "string") {
    throw new Error(`footer text=${JSON.stringify(text)} (expected string)`);
  }

  // Gate: the footer is a pure function of (buildVersion, isPortableBuild) —
  // see SidebarPanel.qml's buildLabel binding — so compare against the
  // exact expected string. This pins the " · " separator and rejects
  // stray suffixes, which token-containment checks would let through.
  const expectedToken = isPortable ? "Portable" : "Dev";
  const expectedText = buildVersion.length > 0
    ? `${buildVersion} · ${expectedToken}`
    : expectedToken;
  if (text !== expectedText) {
    throw new Error(
      `footer text=${JSON.stringify(text)} (expected ${JSON.stringify(expectedText)}; ` +
      `buildVersion=${JSON.stringify(buildVersion)}, isPortableBuild=${isPortable})`);
  }
});

// --- Sidebar (A7) — the active tile follows currentVisibleApp ---
//
// Spec §2.A A7 (amended 2026-08-28): open fixture A, click Settings, then
// re-click the tile. The tile's highlight is
// `checked: modelData.name === (backend.currentVisibleApp || "")`
// (SidebarPanel.qml:131), so it must stay lit while Settings is front-most:
// a section click only flips the content stack
// (MainContainer::onViewIndexChanged), never currentVisibleApp. The re-click
// goes through launchUIModule → navigateToApps →
// setCurrentActiveSectionIndex(0), returning to the workspace.
//
// The Settings-active gate uses the button's own checked
// (`backend.currentActiveSectionIndex - 1 === index`) plus "section index is
// no longer the workspace 0" — never a hardcoded section number, the sidebar
// layout owns the numbering. The view-section SidebarCircleButtons carry no
// objectName (§4.1 skipped them), so the button is located by text + type.
//
// A5's cleanup closed fixture A's dock, so the "open" step here is a genuine
// open; if a dock were already open the click merely re-activates (A5 pinned
// that as dock-count-neutral) and every gate below still holds. Per the
// amended spec this test ENDS with the dock OPEN — workspace section active,
// dockCount 1, tile lit. No later test asserts welcome-page or dock state,
// and the section-walk tests re-click their own sections regardless.

test("sidebar: active tile follows currentVisibleApp across section switches", async (app) => {
  // Same stable evaluate anchor as A3–A5 — has `backend` in context and
  // survives sidebar delegate churn and section switches.
  let welcome = null;
  await app.waitFor(async () => {
    welcome = await findWelcomePage(app);
    if (!welcome) throw new Error("no WelcomePage instance in the QML tree");
  }, { timeout: 10000, interval: 500, description: "WelcomePage instance to exist" });

  const workspace = await findByObjectName(app.inspector, "workspace");
  if (!workspace) {
    throw new Error('WorkspaceArea (objectName "workspace") not found');
  }

  const evalOnWelcome = async (expression) => {
    const res = await app.inspector.send("evaluate", {
      objectId: welcome.id, expression,
    });
    if (res.error) throw new Error(`evaluate(${expression}) failed: ${res.error}`);
    return res.result;
  };

  // Loading moves the tile from the unloaded to the loaded Repeater (same
  // objectName, new object — and only the loaded delegate carries the
  // checked binding), so every checked read re-finds the delegate.
  const tileChecked = async () => {
    const t = await findByObjectName(app.inspector, `sidebar.app.${FIXTURE_A.name}`);
    if (!t) throw new Error(`sidebar.app.${FIXTURE_A.name} not in the tree`);
    const res = await app.inspector.send("evaluate", {
      objectId: t.id, expression: "checked",
    });
    if (res.error) throw new Error(`evaluate(tile checked) failed: ${res.error}`);
    return res.result;
  };

  // Step 1 — open fixture A (CI-skip contract as in A3/A5).
  let tile = null;
  try {
    await app.waitFor(async () => {
      tile = await findByObjectName(app.inspector, `sidebar.app.${FIXTURE_A.name}`);
      if (!tile) throw new Error(`sidebar.app.${FIXTURE_A.name} not in the tree`);
    }, { timeout: 10000, interval: 500, description: "fixture A sidebar tile to appear" });
  } catch (e) {
    if (!CI_MODE) {
      console.log(
        `    SKIP: fixture A (${FIXTURE_A.name}) is not installed in this ` +
        `app instance (spec §0.A: skip, not fail, outside --ci)`);
      return;
    }
    throw new Error(
      `fixture A sidebar tile never appeared — integration-test pre-seeds ` +
      `${FIXTURE_A.name} at boot, so this is a real failure: ${e.message}`);
  }
  const opened = await app.inspector.send("callMethod", {
    objectId: tile.id, method: "clicked",
  });
  if (opened.error) {
    throw new Error(`clicking sidebar.app.${FIXTURE_A.name} failed: ${opened.error}`);
  }

  await app.waitFor(async () => {
    const count = await app.inspector.send("evaluate", {
      objectId: workspace.id, expression: "dockCount",
    });
    if (count.error) throw new Error(`evaluate(dockCount) failed: ${count.error}`);
    if (count.result !== 1) {
      throw new Error(`WorkspaceArea.dockCount=${count.result} (expected 1)`);
    }
    const visibleApp = await evalOnWelcome("backend.currentVisibleApp");
    if (visibleApp !== FIXTURE_A.name) {
      throw new Error(
        `backend.currentVisibleApp=${JSON.stringify(visibleApp)} ` +
        `(expected "${FIXTURE_A.name}")`);
    }
    const section = await evalOnWelcome("backend.currentActiveSectionIndex");
    if (section !== 0) {
      throw new Error(
        `backend.currentActiveSectionIndex=${section} ` +
        `(expected workspace index 0 after open)`);
    }
  }, { timeout: 10000, interval: 500,
       description: "fixture A to open front-most in the workspace" });

  // Gate: tile lit after open. Inside a waitFor — during the load the find
  // can transiently hit the outgoing unloaded delegate (default unchecked).
  await app.waitFor(async () => {
    if ((await tileChecked()) !== true) {
      throw new Error("tile checked=false after open (expected true)");
    }
  }, { timeout: 5000, interval: 250, description: "tile to light up after open" });

  // Step 2 — click the Settings section button. Located by text + type: a
  // bare text click can land on a shallower same-text widget (see the
  // sidebarSection note above), and we need the button object anyway to read its
  // checked. Signal-level click, as everywhere else in the A-series.
  let settingsButton = null;
  await app.waitFor(async () => {
    const hits = await app.findByProperty("text", "Settings");
    settingsButton = (hits.matches ?? [])
      .find((m) => (m.type ?? "").includes("SidebarCircleButton")) || null;
    if (!settingsButton) {
      throw new Error('sidebar "Settings" SidebarCircleButton not found');
    }
  }, { timeout: 10000, interval: 500, description: '"Settings" sidebar button to exist' });

  const settingsChecked = async () => {
    const res = await app.inspector.send("evaluate", {
      objectId: settingsButton.id, expression: "checked",
    });
    if (res.error) {
      throw new Error(`evaluate(Settings button checked) failed: ${res.error}`);
    }
    return res.result;
  };

  const clickedSettings = await app.inspector.send("callMethod", {
    objectId: settingsButton.id, method: "clicked",
  });
  if (clickedSettings.error) {
    throw new Error(`clicking the Settings button failed: ${clickedSettings.error}`);
  }

  await app.waitFor(async () => {
    const section = await evalOnWelcome("backend.currentActiveSectionIndex");
    if (section === 0) {
      throw new Error("still on workspace section 0 after the Settings click");
    }
    if ((await settingsChecked()) !== true) {
      throw new Error("Settings button checked=false with Settings active");
    }
  }, { timeout: 10000, interval: 500, description: "Settings section to become active" });

  // Gate: currentVisibleApp untouched by the section switch, tile STILL lit.
  // Single-shot reads on purpose — a retried wait would mask a transient
  // un-light, and "STILL true" is exactly what this test pins down.
  const visibleInSettings = await evalOnWelcome("backend.currentVisibleApp");
  if (visibleInSettings !== FIXTURE_A.name) {
    throw new Error(
      `backend.currentVisibleApp=${JSON.stringify(visibleInSettings)} after ` +
      `the Settings click (expected it to stay "${FIXTURE_A.name}")`);
  }
  if ((await tileChecked()) !== true) {
    throw new Error(
      "tile checked=false while Settings is active — the tile must track " +
      "currentVisibleApp, not the active section");
  }

  // Step 3 — re-click the tile: back to the workspace. Re-find inside the
  // retry loop as in A5 — a duplicate activation click is harmless.
  await app.waitFor(async () => {
    const t = await findByObjectName(app.inspector, `sidebar.app.${FIXTURE_A.name}`);
    if (!t) throw new Error(`sidebar.app.${FIXTURE_A.name} not in the tree`);
    const reclicked = await app.inspector.send("callMethod", {
      objectId: t.id, method: "clicked",
    });
    if (reclicked.error) {
      throw new Error(`re-clicking sidebar.app.${FIXTURE_A.name} failed: ${reclicked.error}`);
    }
  }, { timeout: 10000, interval: 500, description: "re-click on fixture A tile" });

  await app.waitFor(async () => {
    const section = await evalOnWelcome("backend.currentActiveSectionIndex");
    if (section !== 0) {
      throw new Error(
        `backend.currentActiveSectionIndex=${section} ` +
        `(expected 0 after re-clicking the tile)`);
    }
  }, { timeout: 10000, interval: 500, description: "workspace section to reactivate" });

  // Gates after the re-click: tile still lit, Settings button unchecked,
  // and the suite-visible end state — dockCount still 1, fixture A still
  // front-most in the workspace section.
  if ((await tileChecked()) !== true) {
    throw new Error("tile checked=false after returning to the workspace (expected true)");
  }
  if ((await settingsChecked()) !== false) {
    throw new Error("Settings button still checked after returning to the workspace");
  }
  const finalCount = await app.inspector.send("evaluate", {
    objectId: workspace.id, expression: "dockCount",
  });
  if (finalCount.error) throw new Error(`evaluate(dockCount) failed: ${finalCount.error}`);
  if (finalCount.result !== 1) {
    throw new Error(
      `WorkspaceArea.dockCount=${finalCount.result} at test end (expected 1)`);
  }
  const finalVisible = await evalOnWelcome("backend.currentVisibleApp");
  if (finalVisible !== FIXTURE_A.name) {
    throw new Error(
      `backend.currentVisibleApp=${JSON.stringify(finalVisible)} at test end ` +
      `(expected "${FIXTURE_A.name}")`);
  }
});

// --- App Manager (A8) — search narrows the grid to matching apps ---
//
// Spec §2.A A8 (amended 2026-08-28): Applications → type fixture A's display
// name in deliberately wrong case into appManager.searchField, append a
// non-matching suffix, then clear. AppsFilterProxy's search is a fixed-string
// case-insensitive contains over Name/DisplayName/Description
// (AppsFilterProxy.cpp:327-335).
//
// The spec's literal query is "LIFECYCLE" ("Lifecycle Demo"), but that is the
// doctest package's display name (doctests/basecamp-package-lifecycle
// .test.yaml), NOT this branch's fixture A: the pre-seeded fixture is
// displayName "Test QML Only" (FIXTURE_A in tests/fixtures/lgx.mjs, seeded
// verbatim by nix/integration-test.nix), so "LIFECYCLE" would match nothing.
// The query is therefore DERIVED from FIXTURE_A.displayName, upper-cased to
// keep the spec's wrong-case intent. It still matches via DisplayNameRole
// only: the name is the underscored "test_qml_only" and the seeded
// description embeds that underscored form, so neither contains the spaced
// display name.
//
// Offline the grid has no catalog rows, so fixture A's local row is the only
// one; appManager.localAppsProxy chains matchLocalOnly on top of the OUTER
// searched proxy (AppManagerView.qml:69-75), so its visibleCount tracks the
// search. The spec's non-match gate "backend.uiAppsProxy.rowCount() === 0"
// reads that outer proxy — uiAppsProxy is a ContentViews.qml id, not a
// backend property, and it is exactly localAppsProxy.sourceModel, so the
// gate evaluates sourceModel.rowCount() on the proxy anchor (same access
// path as the matchLocalOnly wiring test).
//
// The field's text is set via inspector evaluate on appManager.searchField
// and read back (round-trip; evaluate returns primitives only, one property
// per call). The assignment breaks the `text: d.searchText` binding, which
// is harmless: onTextChanged pushes the value into d.searchText (the actual
// filter input), and nothing else writes d.searchText. Cleanup: the search
// ends cleared, so appManager.emptyView ends hidden.

test("app manager: search narrows the grid to matching apps", async (app) => {
  await app.click("Applications");
  await app.waitFor(
    async () => { await app.expectTexts(["Install and manage applications."]); },
    { timeout: 10000, interval: 500, description: "Applications view to render" }
  );

  let proxyId = null;
  await app.waitFor(async () => {
    proxyId = await findLocalAppsProxy(app);
    if (proxyId === null) {
      throw new Error("appManager.localAppsProxy not found in QML tree");
    }
  }, { timeout: 10000, interval: 500, description: "localAppsProxy to exist" });

  let field = null;
  await app.waitFor(async () => {
    field = await findByObjectName(app.inspector, "appManager.searchField");
    if (!field) throw new Error("appManager.searchField not in the QML tree");
  }, { timeout: 10000, interval: 500, description: "search field to exist" });

  const setSearch = async (value) => {
    const res = await app.inspector.send("evaluate", {
      objectId: field.id, expression: `text = ${JSON.stringify(value)}`,
    });
    if (res.error) {
      throw new Error(
        `setting search text to ${JSON.stringify(value)} failed: ${res.error}`);
    }
  };

  // Normalize: pre-search means an empty field. Nothing before A8 touches the
  // search, but a leftover value would skew the recording below.
  const initialText = await evalOn(app, field.id, "text");
  if (typeof initialText !== "string") {
    throw new Error(
      `search field text=${JSON.stringify(initialText)} (expected string)`);
  }
  if (initialText !== "") await setSearch("");

  // PRECONDITION (spec gate): fixture A's local row is the one grid row.
  // If it never shows, the seeding produced no user-install row — that is a
  // hard failure in --ci (integration-test pre-seeds fixture A at boot) and
  // a spec-§0.A skip against a local app without the fixture.
  try {
    await app.waitFor(async () => {
      const count = await evalOn(app, proxyId, "visibleCount");
      if (count !== 1) {
        throw new Error(
          `localAppsProxy.visibleCount=${count} ` +
          `(expected exactly 1: fixture A's local row)`);
      }
    }, { timeout: 10000, interval: 500,
         description: "fixture A's local row to be the one grid row" });
  } catch (e) {
    if (!CI_MODE) {
      console.log(
        `    SKIP: A8 precondition localAppsProxy.visibleCount === 1 not met ` +
        `— fixture A (${FIXTURE_A.name}) is not the sole local row in this ` +
        `app instance (spec §0.A: skip, not fail, outside --ci)`);
      return;
    }
    throw new Error(
      `A8 precondition failed — the seeding produced no user-install row ` +
      `(fixture A's local row never became the one grid row): ${e.message}`);
  }

  // Record the pre-search values; the post-clear gate compares against these.
  const preLocal = await evalOn(app, proxyId, "visibleCount");
  const preOuterRows = await evalOn(app, proxyId, "sourceModel.rowCount()");
  if (typeof preOuterRows !== "number") {
    throw new Error(
      `outer proxy rowCount()=${JSON.stringify(preOuterRows)} (expected number)`);
  }

  // Step 1 — the display name in deliberately wrong case: count stays put and
  // the text round-trips (match is case-insensitive over name/displayName/
  // description). Guard that upper-casing actually changed the case — an
  // already-uppercase display name would make this leg assert nothing.
  const matchQuery = FIXTURE_A.displayName.toUpperCase();
  if (matchQuery === FIXTURE_A.displayName) {
    throw new Error(
      `FIXTURE_A.displayName=${JSON.stringify(FIXTURE_A.displayName)} is ` +
      `already upper-case — the wrong-case leg cannot prove case-insensitivity`);
  }
  await setSearch(matchQuery);
  await app.waitFor(async () => {
    const text = await evalOn(app, field.id, "text");
    if (text !== matchQuery) {
      throw new Error(
        `search text=${JSON.stringify(text)} did not round-trip ` +
        `(expected ${JSON.stringify(matchQuery)})`);
    }
    const count = await evalOn(app, proxyId, "visibleCount");
    if (count !== preLocal) {
      throw new Error(
        `localAppsProxy.visibleCount=${count} with wrong-case display-name ` +
        `search (expected it to stay ${preLocal} — search must be ` +
        `case-insensitive)`);
    }
  }, { timeout: 5000, interval: 250,
       description: "wrong-case display-name search to keep fixture A visible" });

  // Step 2 — append a non-matching suffix: the grid empties, all the way down
  // to the outer proxy (the spec's uiAppsProxy — localAppsProxy.sourceModel).
  const noMatchQuery = `${matchQuery} ZZZ-NO-SUCH-APP`;
  await setSearch(noMatchQuery);
  await app.waitFor(async () => {
    const text = await evalOn(app, field.id, "text");
    if (text !== noMatchQuery) {
      throw new Error(
        `search text=${JSON.stringify(text)} did not round-trip ` +
        `(expected ${JSON.stringify(noMatchQuery)})`);
    }
    const count = await evalOn(app, proxyId, "visibleCount");
    if (count !== 0) {
      throw new Error(
        `localAppsProxy.visibleCount=${count} with non-matching search ` +
        `(expected 0)`);
    }
    const outerRows = await evalOn(app, proxyId, "sourceModel.rowCount()");
    if (outerRows !== 0) {
      throw new Error(
        `outer apps proxy rowCount()=${outerRows} with non-matching search ` +
        `(expected 0)`);
    }
  }, { timeout: 5000, interval: 250,
       description: "non-matching search to empty the grid" });

  // Step 3 — clear: the recorded pre-search values return, and the
  // empty-search view ends hidden (its `visible` binds on
  // d.searchText.length > 0).
  await setSearch("");
  await app.waitFor(async () => {
    const text = await evalOn(app, field.id, "text");
    if (text !== "") {
      throw new Error(`search text=${JSON.stringify(text)} after clear (expected "")`);
    }
    const count = await evalOn(app, proxyId, "visibleCount");
    if (count !== preLocal) {
      throw new Error(
        `localAppsProxy.visibleCount=${count} after clearing the search ` +
        `(expected the recorded pre-search ${preLocal})`);
    }
    const outerRows = await evalOn(app, proxyId, "sourceModel.rowCount()");
    if (outerRows !== preOuterRows) {
      throw new Error(
        `outer apps proxy rowCount()=${outerRows} after clearing the search ` +
        `(expected the recorded pre-search ${preOuterRows})`);
    }
    const empty = await findByObjectName(app.inspector, "appManager.emptyView");
    if (!empty) throw new Error("appManager.emptyView not in the QML tree");
    const emptyVisible = await evalOn(app, empty.id, "visible");
    if (emptyVisible !== false) {
      throw new Error(
        `appManager.emptyView visible=${emptyVisible} after clearing the ` +
        `search (expected false)`);
    }
  }, { timeout: 5000, interval: 250,
       description: "cleared search to restore the pre-search grid" });
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
async function openAppsInspector(app) {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Apps Inspector", "Module Inspector"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
  await app.click("Apps Inspector", { type: "LogosItemDelegate" });
  await app.waitFor(
    async () => { await app.expectTexts(["UI plugins available in this installation."]); },
    { timeout: 10000, interval: 500, description: "Apps Inspector to become active" }
  );
}

async function openModuleInspector(app) {
  await app.click("Settings");
  await app.waitFor(
    async () => { await app.expectTexts(["Dashboard", "Apps Inspector", "Module Inspector"]); },
    { timeout: 10000, interval: 500, description: "Settings entries to render" }
  );
  await app.click("Module Inspector", { type: "LogosItemDelegate" });
  await app.waitFor(
    async () => { await app.expectTexts(["Core modules known to the runtime, with live resource usage."]); },
    { timeout: 10000, interval: 500, description: "Module Inspector to become active" }
  );
}

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

async function invoke(app, objectId, method) {
  const res = await app.inspector.send("callMethod", { objectId, method });
  if (res.error) throw new Error(`callMethod(${method}) failed: ${res.error}`);
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
