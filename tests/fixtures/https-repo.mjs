// A package repository served over local HTTPS, as Basecamp needs one: the
// downloader takes https:// repositories and package URLs only. The server
// presents a throwaway self-signed certificate, which the app is told to
// trust next to the public ones through SSL_CERT_FILE (env()).
//
// Needs the openssl CLI to make the certificate; serveRepository() throws
// without it.

import { createServer } from "node:https";
import { execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { basename, join, resolve } from "node:path";

import { readLgxManifest } from "./lgx.mjs";

// The public certificates the app keeps trusting: SSL_CERT_FILE replaces the
// trusted set rather than adding to it.
function publicBundle() {
  const candidates = [
    process.env.LOGOS_TEST_CA_BUNDLE,
    process.env.NIX_SSL_CERT_FILE,
    process.env.SSL_CERT_FILE,
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/ssl/cert.pem",
  ];
  for (const path of candidates) {
    if (path && existsSync(path)) return readFileSync(path, "utf-8");
  }
  return "";
}

// Dotted numbers compared as numbers: enough for the fixtures' versions.
function compareVersions(a, b) {
  const x = String(a).split(".").map(Number);
  const y = String(b).split(".").map(Number);
  for (let i = 0; i < Math.max(x.length, y.length); ++i) {
    const d = (x[i] || 0) - (y[i] || 0);
    if (d !== 0) return d;
  }
  return 0;
}

// A key and a certificate for 127.0.0.1 in `dir`.
function makeCertificate(dir) {
  const key = join(dir, "localhost.key");
  const cert = join(dir, "localhost.crt");
  execFileSync("openssl", ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                           "-keyout", key, "-out", cert, "-subj", "/CN=localhost",
                           "-addext", "subjectAltName=IP:127.0.0.1"], { stdio: "ignore" });
  return { key, cert };
}

// Serves `packages` (.lgx paths; several of one name are its versions) as a
// repository from `dir`. Resolves to
// { url (its logos-repo.json), env (for spawnBasecamp), requests (every path
// asked for, in order), republish(lgxPath) (serves it in place of the package
// of its name), close() }. `opts.stall`: paths it never answers, a download
// that stays on its way.
export async function serveRepository(dir, packages, opts = {}) {
  const { name = "local-test-repo", displayName = "Local Test Repo", stall = [] } = opts;
  const www = join(dir, "www");
  mkdirSync(www, { recursive: true });
  const { key, cert } = makeCertificate(dir);

  const requests = [];
  const server = createServer({ key: readFileSync(key), cert: readFileSync(cert) }, (req, res) => {
    const asked = decodeURIComponent(new URL(req.url, "https://x").pathname);
    requests.push(asked);
    if (stall.includes(asked)) return;
    const path = resolve(www, "." + asked);
    if (!path.startsWith(www + "/") || !existsSync(path)) {
      res.writeHead(404);
      res.end();
      return;
    }
    res.writeHead(200);
    res.end(readFileSync(path));
  });
  await new Promise((done) => server.listen(0, "127.0.0.1", done));
  const base = `https://127.0.0.1:${server.address().port}`;

  const rowFor = (lgxPath) => {
    const file = basename(lgxPath);
    copyFileSync(lgxPath, join(www, file));
    const manifest = readLgxManifest(lgxPath);
    return {
      name: manifest.name,
      repositoryUrl: `${base}/logos-repo.json`,
      versions: [{ rootHash: manifest.hashes?.root ?? "", url: `${base}/${file}`, manifest }],
    };
  };
  // Several packages of one name are its versions, newest first, as the
  // catalog lists them.
  const rows = [];
  for (const row of packages.map(rowFor)) {
    const same = rows.find((r) => r.name === row.name);
    if (!same) {
      rows.push(row);
      continue;
    }
    same.versions.push(...row.versions);
    same.versions.sort((a, b) => compareVersions(b.manifest.version, a.manifest.version));
  }
  const writeIndex = () =>
    writeFileSync(join(www, "index.json"), JSON.stringify({ packages: rows }, null, 2) + "\n");
  writeIndex();
  writeFileSync(join(www, "logos-repo.json"),
                JSON.stringify({ schemaVersion: 1, name, displayName, description: displayName,
                                 indexUrl: `${base}/index.json`, trustedSigners: [] }, null, 2) + "\n");

  const trust = join(dir, "trust.pem");
  writeFileSync(trust, publicBundle() + "\n" + readFileSync(cert, "utf-8"));
  return {
    url: `${base}/logos-repo.json`,
    env: { SSL_CERT_FILE: trust },
    requests,
    republish: (lgxPath) => {
      const row = rowFor(lgxPath);
      rows[rows.findIndex((r) => r.name === row.name)] = row;
      writeIndex();
    },
    close: () => new Promise((done) => {
      server.close(done);
      server.closeAllConnections();
    }),
  };
}
