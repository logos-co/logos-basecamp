// LGX fixture generator: an .lgx is a gzipped ustar tar (manifest.json + payload
// under variants/<platform>/); the lgx CLI (LGX_CLI) stamps the required content
// hashes. Core-module fixtures are manifest-level only, not loadable binaries.

import { execFileSync } from "node:child_process";
import { mkdirSync, writeFileSync, rmSync, existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
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
  execFileSync(lgx, ["add", outFile, "--variant", variant, "--files", payload, "-y"]);
  if (verify) execFileSync(lgx, ["verify", outFile]);

  rmSync(work, { recursive: true, force: true });
  return outFile;
}

// Valid tar, corrupt manifest — deliberately NOT run through lgx (it would refuse).
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

export function readLgxManifest(lgxPath) {
  const out = execFileSync("tar", ["-xzOf", lgxPath, "manifest.json"],
    { encoding: "utf-8" });
  return JSON.parse(out);
}

export function makeFixtureSet(outDir, opts = {}) {
  mkdirSync(outDir, { recursive: true });
  const version = opts.version || "0.1.0";
  const specs = {
    // A→X,Y / B→Y: uninstalling Y cascades to both apps, X only to A.
    mod_x: { type: "core",   displayName: "Fixture Module X", dependencies: [] },
    mod_y: { type: "core",   displayName: "Fixture Module Y", dependencies: [] },
    app_a: { type: "ui_qml", displayName: "Fixture App A",    dependencies: ["mod_x", "mod_y"] },
    app_b: { type: "ui_qml", displayName: "Fixture App B",    dependencies: ["mod_y"] },
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

// Fixture A (spec §0.A/§4.7): the standing launcher app pre-seeded at boot —
// the target for app-tile/dock/context-menu tests.
export const FIXTURE_A = {
  name: "test_qml_only",
  version: "0.1.1",
  displayName: "Test QML Only",
  type: "ui_qml",
  dependencies: [],
};

// Pre-boot seeding (spec §4.7): getInstalledUiPlugins/getInstalledPackages are
// live directory scans for <name>/manifest.json — no install registry — so a
// fixture extracted into <user-dir>/plugins/ is indistinguishable from an
// installed one, at boot and on every refresh().
export function seedPlugin(userDir, fixture) {
  return writeInstalledPlugin(userDir, fixture);
}

// Seed an installed package into a --user-dir; the scanner only reads manifest.json.
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

// Local file:// repo: logos-repo.json + index listing the given .lgx files.
export function writeLocalRepo(dir, opts = {}) {
  const { name = "Local Test Repo", packages = [] } = opts;
  mkdirSync(dir, { recursive: true });

  const indexPath = join(dir, "index.json");
  const repoPath = join(dir, "logos-repo.json");
  const indexUrl = pathToFileURL(indexPath).href;
  const repoUrl = pathToFileURL(repoPath).href;

  const rows = packages.map((lgxPath) => {
    const manifest = readLgxManifest(lgxPath);
    return {
      name: manifest.name,
      repositoryUrl: repoUrl,
      versions: [{
        rootHash: manifest.hashes?.root ?? "",
        url: pathToFileURL(resolve(lgxPath)).href,
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
