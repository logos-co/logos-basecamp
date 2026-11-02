// Install a hand-written app directory into a --user-dir the way lgpm does,
// content hashes included -- for shell scripts that stage fixtures.
//
// Usage: node install-fixture.mjs <user-dir> <app-dir>
//   <app-dir> holds the package's manifest.json beside the files to install.
// Needs LGX_CLI (the lgx CLI).

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { installFixture } from "./lgx.mjs";

const [userDir, appDir] = process.argv.slice(2);
if (!userDir || !appDir) {
  console.error("usage: node install-fixture.mjs <user-dir> <app-dir>");
  process.exit(2);
}
const manifest = JSON.parse(readFileSync(join(appDir, "manifest.json"), "utf8"));
console.log(installFixture(userDir, manifest, appDir));
