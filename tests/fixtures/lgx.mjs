// ---------------------------------------------------------------------------
// LGX fixture generator for the basecamp test suites.
//
// An .lgx is a gzipped ustar tar: manifest.json at the root plus a payload
// under variants/<platform>/. This is a port of the inline shell generator in
// doctests/basecamp-package-lifecycle.test.yaml — hand-write the stamped
// manifest + payload, seed a plain-ustar tar, then let the `lgx` CLI inject
// the payload and stamp the manifest's content hashes (packages without them
// fail validation).
//
// Requires the lgx CLI for valid packages:
//   nix build 'github:logos-co/logos-package#lgx' -o result-lgx
// Override its location with LGX_CLI (default: <repo>/result-lgx/bin/lgx).
//
// Named fixtures (makeFixtureSet):
//   app_a  → depends on mod_x, mod_y   ┐  the A→X,Y / B→Y trio for
//   app_b  → depends on mod_y          ┘  dependency-cascade flows
//   mod_x, mod_y                       core modules the apps depend on
//   mod_z  → depends on no_such_module (a name nothing provides)
//   bad_app                            corrupt manifest (invalid JSON)
//
// plus writeLocalRepo() — a local logos-repo.json (+ file:// package URLs)
// for repository-driven install flows.
//
// The core-module fixtures are manifest-level: they give the resolver a
// package identity and dependency edges, not a loadable binary (a compiled
// lib can't be hand-crafted here). That is all the dependency/cascade gates
// observe.
// ---------------------------------------------------------------------------

import { execFileSync } from "node:child_process";
import { mkdirSync, writeFileSync, rmSync, existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(__dirname, "../..");

export const LGX_CLI =
  process.env.LGX_CLI || resolve(projectRoot, "result-lgx/bin/lgx");

// The portable bundle installs plain variant names — no `-dev` suffix.
export function platformVariant() {
  const key = `${process.platform} ${process.arch}`;
  const map = {
    "linux x64":    "linux-x86_64",
    "linux arm64":  "linux-arm64",
    "darwin arm64": "darwin-arm64",
    "darwin x64":   "darwin-x86_64",
  };
  const variant = map[key];
  if (!variant) throw new Error(`unsupported platform: ${key}`);
  return variant;
}

function manifestFor({ name, displayName, version, type, dependencies, description }) {
  const manifest = {
    author: "",
    category: "testing",
    dependencies,
    description,
    display_name: displayName,
    icon: "",
    main: {},
    manifestVersion: "0.3.0",
    name,
    type,
    version,
  };
  if (type === "ui_qml") manifest.view = "Main.qml";
  return manifest;
}

function metadataFor({ name, displayName, version, type, dependencies, description }) {
  const metadata = {
    name,
    display_name: displayName,
    version,
    type,
    category: "testing",
    description,
    dependencies,
  };
  if (type === "ui_qml") metadata.view = "Main.qml";
  return metadata;
}

function qmlViewFor({ name, displayName, version }) {
  // The view prints its own identity + version so a test can assert which
  // payload actually runs after an install or upgrade.
  return `import QtQuick

Rectangle {
    id: root
    color: "#1e1e1e"
    Text {
        anchors.centerIn: parent
        text: "${displayName} (${name}) v${version}"
        color: "#ffffff"
        font.pixelSize: 24
    }
}
`;
}

// Write the on-disk payload files for one package version into `payloadDir`.
// For ui_qml that is metadata.json + Main.qml + qmldir; for core modules
// just metadata.json (manifest-level fixture — see the header).
export function writePayload(payloadDir, spec) {
  mkdirSync(payloadDir, { recursive: true });
  writeFileSync(join(payloadDir, "metadata.json"),
    JSON.stringify(metadataFor(spec), null, 2) + "\n");
  if (spec.type === "ui_qml") {
    writeFileSync(join(payloadDir, "Main.qml"), qmlViewFor(spec));
    writeFileSync(join(payloadDir, "qmldir"),
      `module com.logos.module.${spec.name}\n`);
  }
}

// ---------------------------------------------------------------------------
// makeLgx — build one valid .lgx at outFile.
//
//   makeLgx({
//     outFile: "/tmp/fixtures/app_a-0.1.0.lgx",
//     name: "app_a", displayName: "App A", version: "0.1.0",
//     type: "ui_qml",              // or "core"
//     dependencies: ["mod_x"],
//     verify: true,                // run `lgx verify` afterwards
//   })
// ---------------------------------------------------------------------------
export function makeLgx(opts) {
  const {
    outFile,
    name,
    version = "0.1.0",
    displayName = name,
    type = "ui_qml",
    dependencies = [],
    description = `basecamp test fixture package (${name})`,
    lgx = LGX_CLI,
    variant = platformVariant(),
    verify = true,
  } = opts;
  if (!outFile || !name) throw new Error("makeLgx: outFile and name are required");
  if (!existsSync(lgx)) {
    throw new Error(
      `makeLgx: lgx CLI not found at ${lgx} — build it with ` +
      `"nix build 'github:logos-co/logos-package#lgx' -o result-lgx" ` +
      `or set LGX_CLI`);
  }
  const spec = { name, displayName, version, type, dependencies, description };

  const work = join(tmpdir(), `lgx-build-${process.pid}-${name}-${version}`);
  const seed = join(work, "seed");
  const payload = join(work, "payload");
  rmSync(work, { recursive: true, force: true });
  mkdirSync(join(seed, "variants"), { recursive: true });
  writeFileSync(join(seed, "manifest.json"),
    JSON.stringify(manifestFor(spec), null, 2) + "\n");
  writePayload(payload, spec);

  mkdirSync(dirname(outFile), { recursive: true });
  // Plain ustar — the LGX reader doesn't speak pax extended headers.
  execFileSync("tar",
    ["--format", "ustar", "-C", seed, "-czf", outFile, "manifest.json", "variants"]);
  // lgx add injects the payload into the platform variant and stamps the
  // manifest's content hashes (the Merkle root packages must carry).
  execFileSync(lgx, ["add", outFile, "--variant", variant, "--files", payload, "-y"]);
  if (verify) execFileSync(lgx, ["verify", outFile]);

  rmSync(work, { recursive: true, force: true });
  return outFile;
}

// bad_app — a syntactically broken package: valid gzipped ustar tar, corrupt
// manifest.json (truncated JSON). Exercises the reject/error paths, so it is
// deliberately NOT run through lgx (which would refuse to touch it).
export function makeBadApp(outDir) {
  const outFile = join(outDir, "bad_app.lgx");
  const work = join(tmpdir(), `lgx-build-${process.pid}-bad_app`);
  rmSync(work, { recursive: true, force: true });
  mkdirSync(join(work, "variants"), { recursive: true });
  writeFileSync(join(work, "manifest.json"),
    '{ "name": "bad_app", "version": "0.1.0", "type": "ui_qml", "depende');
  mkdirSync(outDir, { recursive: true });
  execFileSync("tar",
    ["--format", "ustar", "-C", work, "-czf", outFile, "manifest.json", "variants"]);
  rmSync(work, { recursive: true, force: true });
  return outFile;
}

// Read the (possibly hash-stamped) manifest back out of an .lgx.
export function readLgxManifest(lgxPath) {
  const out = execFileSync("tar", ["-xzOf", lgxPath, "manifest.json"],
    { encoding: "utf-8" });
  return JSON.parse(out);
}

// ---------------------------------------------------------------------------
// makeFixtureSet — build the whole named set into outDir. Returns
// { app_a, app_b, mod_x, mod_y, mod_z, bad_app } → absolute .lgx paths.
// ---------------------------------------------------------------------------
export function makeFixtureSet(outDir, opts = {}) {
  mkdirSync(outDir, { recursive: true });
  const version = opts.version || "0.1.0";
  const specs = {
    // The A→X,Y / B→Y trio: uninstalling Y cascades to both apps,
    // uninstalling X only to A.
    mod_x: { type: "core",   displayName: "Fixture Module X", dependencies: [] },
    mod_y: { type: "core",   displayName: "Fixture Module Y", dependencies: [] },
    app_a: { type: "ui_qml", displayName: "Fixture App A",    dependencies: ["mod_x", "mod_y"] },
    app_b: { type: "ui_qml", displayName: "Fixture App B",    dependencies: ["mod_y"] },
    // Z depends on a name no module anywhere provides.
    mod_z: { type: "core",   displayName: "Fixture Module Z", dependencies: ["no_such_module"] },
  };
  const paths = {};
  for (const [name, spec] of Object.entries(specs)) {
    paths[name] = makeLgx({
      outFile: join(outDir, `${name}-${version}.lgx`),
      name, version, ...spec, ...opts,
    });
  }
  paths.bad_app = makeBadApp(outDir);
  return paths;
}

// ---------------------------------------------------------------------------
// writeInstalledPlugin — seed an already-installed package directly into a
// --user-dir (plugins/ for ui_qml, modules/ for core), the way the
// missing-deps doctest crafts broken end states. The package scanner only
// reads manifest.json, so this is indistinguishable from a real install.
// ---------------------------------------------------------------------------
export function writeInstalledPlugin(userDir, spec) {
  const full = {
    version: "0.1.0",
    displayName: spec.name,
    type: "ui_qml",
    dependencies: [],
    description: `basecamp test fixture package (${spec.name})`,
    ...spec,
  };
  const bucket = full.type === "core" ? "modules" : "plugins";
  const dir = join(userDir, bucket, full.name);
  writePayload(dir, full);
  writeFileSync(join(dir, "manifest.json"),
    JSON.stringify(manifestFor(full), null, 2) + "\n");
  return dir;
}

// ---------------------------------------------------------------------------
// writeLocalRepo — a local repository the app can add by file:// URL:
// logos-repo.json (name + indexUrl) next to an index listing the given .lgx
// files with file:// download URLs. Row shape mirrors the default repo's
// catalog index (name / versions[] / rootHash / manifest — see
// tests/apps_model_test.cpp makeCatalogRow).
//
//   const { repoUrl } = writeLocalRepo(dir, { packages: [paths.app_a] });
// ---------------------------------------------------------------------------
export function writeLocalRepo(dir, opts = {}) {
  const { name = "Local Test Repo", packages = [] } = opts;
  mkdirSync(dir, { recursive: true });

  const indexPath = join(dir, "index.json");
  const repoPath = join(dir, "logos-repo.json");
  const indexUrl = `file://${indexPath}`;
  const repoUrl = `file://${repoPath}`;

  const rows = packages.map((lgxPath) => {
    const manifest = readLgxManifest(lgxPath);
    return {
      name: manifest.name,
      repositoryUrl: repoUrl,
      versions: [{
        rootHash: manifest.hashes?.root ?? "",
        url: `file://${resolve(lgxPath)}`,
        manifest,
      }],
    };
  });

  writeFileSync(indexPath,
    JSON.stringify({ packages: rows }, null, 2) + "\n");
  writeFileSync(repoPath,
    JSON.stringify({ name, description: name, indexUrl }, null, 2) + "\n");
  return { repoUrl, indexUrl, repoPath, indexPath };
}
