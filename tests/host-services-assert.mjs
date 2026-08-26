// ---------------------------------------------------------------------------
// host-services-assert.mjs — the shared assertion behind the host-services
// grant guard. Imported by tests/host-services-tests.mjs (the dedicated
// `host-services-test` check) and by tests/ui-tests.mjs (so `integration-test`,
// which CI already runs by name, stops being blind to this).
//
// ── What this exists to catch ────────────────────────────────────────────────
//
// capability_module is the trust root: it mints the auth token a NON-"core"
// identity needs before any other module will answer it. To do that job it
// needs two privileged host services, `token_registry` (enumerate the token
// store, so it can verify a caller is a real loaded identity) and
// `token_delivery` (push a token to a target). Those are granted by the HOST —
// QtPluginFormatLoader::buildArguments in logos-module-loader-qt appends
// `--host-services` for capability_module, logos_host stamps it on the LogosAPI
// object, and the plugin's generated glue forwards it across the module-impl C
// ABI into the module's own image.
//
// That is a four-repo chain (basecamp's pins -> logos-liblogos ->
// logos-module-loader-qt -> logos-protocol/logos-plugin-qt), and basecamp
// shipped for a while with a `default-module-loader` pin one commit BEFORE the
// grant existed. capability_module then failed closed, every cross-identity
// call was answered with "ModuleProxy: rejecting unauthorized call", and the
// UI suite still reported 16 passed / 0 failed — because every assertion in it
// was satisfied by chrome that basecamp or PMUI renders unconditionally.
//
// ── Why THIS assertion is not that ───────────────────────────────────────────
//
// `pmui.BackendStore.repositoryCount` is not chrome. Reading it end to end:
//
//   PackageManagerBackend::reload() runs inside ui-host under the identity
//   "package_manager_ui" — NOT "core", so it holds no ambient token and must go
//   through capability_module.requestModule() to get one. It then issues a
//   nested chain of cross-module calls, each gated by ModuleProxy::isAuthorized:
//
//       package_downloader.getCatalog
//         -> package_manager.getInstalledPackages
//           -> package_manager.getValidVariants
//             -> package_downloader.listRepositories
//                  -> setRepositoryCount(repos.size())
//       (logos-package-manager-ui src/PackageManagerBackend.cpp:725-766)
//
//   Two distinct target modules, so the assertion covers the grant for more
//   than one peer, not just one lucky endpoint.
//
//   The property is initialised to 0 (PackageManagerBackend.cpp:67) and is only
//   ever written from that innermost callback. Because the chain is nested,
//   ANY of the four calls being refused means the write never happens and the
//   value stays 0. It reaches the app process as a QtRO replica property
//   (src/package_manager_ui.rep:86) and surfaces as BackendStore.repositoryCount
//   (src/qml/BackendStore.qml:46).
//
// So `repositoryCount >= 1` is a positive statement that a real privileged
// operation SUCCEEDED: a non-core identity was minted a token by
// capability_module and used it to complete four gated calls. Nothing basecamp
// draws can satisfy it.
//
// ── Why repositoryCount and not the more obvious numbers ─────────────────────
//
// MEASURED, both arms, with and without network (dead proxy at 127.0.0.1:9):
//
//   grant reaches, network    repositoryCount=1  totalCount=15 categories=8
//   grant reaches, NO network repositoryCount=1  totalCount=0  categories=1
//   grant blocked,  network   repositoryCount=0  totalCount=0  categories=1
//
// totalCount / categories / availableTypes all derive from the fetched catalog,
// so they collapse to the failure value whenever there is no network — which is
// exactly the situation inside a sandboxed Linux nix builder. Asserting on them
// would be green on a developer's macOS box (where nix `sandbox = false` leaves
// the network open) and red in CI. repositoryCount comes from local repository
// CONFIG, so it is 1 either way and is the only member of that set safe to gate
// on. Do not "strengthen" this by adding totalCount > 0.
// ---------------------------------------------------------------------------

// Click options that pin the click to the sidebar SECTION button. Without the
// type filter, findAndClick's breadth-first walk matches MainContainer's
// "Loading Package Manager…" QLabel first and reports success without opening
// anything — see the long note in ui-tests.mjs.
export const PMUI_SIDEBAR_SECTION = { exact: true, type: "SidebarCircleButton" };

const PMUI_STORE_OBJECT_NAME = "pmui.BackendStore";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * Read every property of PMUI's BackendStore, or null when it is not in the
 * object tree (PMUI not loaded, or renamed).
 */
export async function readPmuiBackendStore(app) {
  const found = await app.inspector.send("findByProperty", {
    property: "objectName",
    value: PMUI_STORE_OBJECT_NAME,
  });
  const matches = found.matches || [];
  if (matches.length === 0) return null;

  const props = await app.inspector.send("getProperties", { objectId: matches[0].id });
  if (props.error) return null;

  const out = {};
  for (const p of props.properties || []) out[p.name] = p.value;
  return out;
}

/**
 * Open the Package Manager section and assert that the host-services grant
 * reached capability_module, by requiring a privileged operation to have
 * actually succeeded.
 *
 * Throws (never skips) on every negative outcome, including "PMUI never
 * loaded" — an unloaded PMUI is an inconclusive run, and an inconclusive run
 * must not certify the build.
 *
 * @returns the BackendStore property snapshot that satisfied the assertion.
 */
export async function assertHostServicesGrantReached(app, opts = {}) {
  const { timeout = 90000, log = () => {} } = opts;

  await app.click("Package Manager", PMUI_SIDEBAR_SECTION);

  const deadline = Date.now() + timeout;
  let everSawStore = false;
  let last = null;

  while (Date.now() < deadline) {
    const store = await readPmuiBackendStore(app);
    if (store) {
      everSawStore = true;
      last = store;
      const n = store.repositoryCount;
      if (typeof n === "number" && n >= 1) {
        log(
          `host-services: capability grant reached — package_manager_ui completed ` +
          `its gated call chain (repositoryCount=${n}, totalCount=${store.totalCount})`
        );
        return store;
      }
    }
    await sleep(1000);
  }

  if (!everSawStore) {
    throw new Error(
      `INCONCLUSIVE, reported as failure: PMUI's "${PMUI_STORE_OBJECT_NAME}" never appeared ` +
      `within ${timeout}ms, so the capability-gated call chain was never exercised and this ` +
      `build cannot be certified. Either the Package Manager section did not open (check the ` +
      `sidebar click), ui-host did not come up, or PMUI renamed the store's objectName — in ` +
      `which case update PMUI_STORE_OBJECT_NAME here rather than deleting the assertion.`
    );
  }

  throw new Error(
    `HOST-SERVICES GRANT DID NOT REACH capability_module.\n` +
    `  pmui.BackendStore.repositoryCount stayed ${JSON.stringify(last.repositoryCount)} ` +
    `for ${timeout}ms (expected >= 1).\n` +
    `  PMUI loaded and its QML is live, so this is not a UI failure: ui-host, running as the ` +
    `non-"core" identity "package_manager_ui", never completed the gated chain\n` +
    `    package_downloader.getCatalog -> package_manager.getInstalledPackages -> ` +
    `package_manager.getValidVariants -> package_downloader.listRepositories.\n` +
    `  The usual cause is that capability_module was not granted ["token_registry",` +
    `"token_delivery"], so it fails closed and mints no token — check that basecamp's ` +
    `logos-liblogos / default-module-loader pin includes the host-services grant, and look ` +
    `for "rejecting unauthorized call" / "was not granted the token_registry host service" ` +
    `in the app log.\n` +
    `  Full store snapshot: ${JSON.stringify(last)}`
  );
}
