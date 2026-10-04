// Source files must be plain ASCII: invisible characters (BOMs, raw control
// bytes inside regexes) have been introduced by tooling before.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");

function* files(dir) {
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) yield* files(path);
    else if (/\.(js|mjs|sql)$/.test(name)) yield path;
  }
}

test("source files contain only printable ASCII", () => {
  const offenders = [];
  for (const path of [...files(join(root, "src")), ...files(join(root, "migrations")), ...files(join(root, "test"))]) {
    if (path.includes(join("test", "legacy"))) continue; // production bundle, kept verbatim
    const bytes = readFileSync(path);
    bytes.forEach((b, i) => {
      if (b > 126 || (b < 32 && b !== 9 && b !== 10 && b !== 13)) offenders.push(`${relative(root, path)} @${i}: 0x${b.toString(16)}`);
    });
  }
  assert.deepEqual(offenders, []);
});
